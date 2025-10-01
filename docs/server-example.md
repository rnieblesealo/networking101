```c
// server_raylib.c — raylib render + network thread (select)
// Focus: server + threading logic using raylib for rendering.
// Assets/text rendering deliberately omitted — call your own routines where marked.
//
// Protocol (big-endian):
//   Client REGISTER (0x01):
//     u8  opcode=0x01
//     u16 tag_len
//     u32 w, u32 h
//     u8  channels (1/3/4)
//     u32 avatar_len == w*h*channels
//     bytes tag[tag_len] (UTF-8, not NUL-terminated)
//     bytes avatar[avatar_len] (raw pixels)
//   Server ACK (0x81): u8 opcode, u32 player_id, s32 pos_x, s32 pos_y
//   Server SHUTDOWN (0xFF): u8 opcode (sent on graceful exit)
//
// Threading model:
//   - Main thread: raylib window + render loop reading shared Player table.
//   - Network thread: select() on listener + clients; parses REGISTER; updates table; ACK/SHUTDOWN.
//   - Shared state protected by a single mutex.
//
// Build (Linux):
//   gcc server_raylib.c -o server_raylib -lraylib -lpthread -lm -ldl -lrt -lX11
// Build (macOS, Homebrew raylib):
//   clang server_raylib.c -o server_raylib -I/usr/local/include -L/usr/local/lib -lraylib -lpthread
// Run:
//   ./server_raylib 0.0.0.0 42424

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "raylib.h"

// ---------- Config ----------
#define MAX_CLIENTS   256
#define MAX_PLAYERS   256
#define MAX_TAG_LEN   31
#define MAX_W         64
#define MAX_H         64
#define WINDOW_W      1024
#define WINDOW_H      640

// ---------- Protocol opcodes ----------
enum { OPC_REGISTER = 0x01, OPC_ACK = 0x81, OPC_SHUTDOWN = 0xFF };

// ---------- Player model ----------
typedef struct {
  uint32_t key_ip;      // IPv4 (network order) used as identity key for demo
  uint32_t player_id;   // monotonic id
  char     tag[MAX_TAG_LEN+1];
  int      pos_x, pos_y;
  uint8_t *avatar;      // raw RGBA32 pixels (converted if needed)
  uint32_t w, h, ch;    // ch is 4 after conversion
  bool     connected;   // currently connected?
  time_t   last_seen;
  Texture2D tex;        // raylib GPU texture
  bool     tex_inited;  // whether tex was initialized
  bool     tex_dirty;   // avatar changed -> reupload to GPU
} Player;

// ---------- Global shared state ----------
static Player g_players[MAX_PLAYERS];
static size_t g_player_count = 0;
static int    g_client_fds[MAX_CLIENTS];
static size_t g_client_count = 0;
static uint32_t g_next_player_id = 1;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_running = true;

// ---------- Utility ----------

// DONE
static int irand(int a, int b) { return a + rand() % (b - a + 1); }

// DONE
static ssize_t recvall(int fd, void *buf, size_t len) {
  uint8_t *p = (uint8_t*)buf; size_t off = 0;
  while (off < len) {
    ssize_t n = recv(fd, p+off, len-off, 0);
    if (n == 0) return 0; // peer closed
    if (n < 0) { if (errno == EINTR) continue; return -1; }
    off += (size_t)n;
  }
  return (ssize_t)off;
}

// DONE
static ssize_t sendall(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t*)buf; size_t off = 0;
  while (off < len) {
    ssize_t n = send(fd, p+off, len-off, 0);
    if (n <= 0) { if (errno == EINTR) continue; return n; }
    off += (size_t)n;
  }
  return (ssize_t)off;
}

// ---------- Player table ops (g_lock required) ----------
static Player* find_player_by_ip(uint32_t key_ip) {
  for (size_t i=0;i<g_player_count;i++) if (g_players[i].key_ip == key_ip) return &g_players[i];
  return NULL;
}

static Player* ensure_player(uint32_t key_ip) {
  Player *p = find_player_by_ip(key_ip);
  if (p) return p;
  if (g_player_count >= MAX_PLAYERS) return NULL;
  Player *np = &g_players[g_player_count++];
  memset(np, 0, sizeof(*np));
  np->key_ip = key_ip;
  np->player_id = g_next_player_id++;
  np->pos_x = irand(16, WINDOW_W - 80);
  np->pos_y = irand(32, WINDOW_H - 80);
  np->connected = false;
  np->tex_inited = false; np->tex_dirty = false;
  return np;
}

static void set_player_avatar(Player *p, const uint8_t *pix, uint32_t w, uint32_t h, uint8_t ch) {
  if (w > MAX_W) w = MAX_W; if (h > MAX_H) h = MAX_H;
  size_t rgba_len = (size_t)w * h * 4;
  uint8_t *dst = (uint8_t*)malloc(rgba_len);
  if (!dst) return;
  for (uint32_t y=0; y<h; ++y) {
    for (uint32_t x=0; x<w; ++x) {
      size_t i = (size_t)y*w + x;
      uint8_t r,g,b,a;
      if (ch == 4) { r = pix[i*4+0]; g = pix[i*4+1]; b = pix[i*4+2]; a = pix[i*4+3]; }
      else if (ch == 3) { r = pix[i*3+0]; g = pix[i*3+1]; b = pix[i*3+2]; a = 255; }
      else { r = g = b = pix[i]; a = 255; }
      dst[i*4+0] = r; dst[i*4+1] = g; dst[i*4+2] = b; dst[i*4+3] = a;
    }
  }
  free(p->avatar); p->avatar = dst; p->w = w; p->h = h; p->ch = 4; p->tex_dirty = true;
}

// ---------- Client handling (REGISTER only) ----------
static bool handle_register(int cfd, uint32_t peer_ip_net) {
  uint8_t op; if (recvall(cfd, &op, 1) <= 0) return false; if (op != OPC_REGISTER) return false;
  uint16_t tag_len_be; uint32_t w_be, h_be, len_be; uint8_t ch;
  if (recvall(cfd, &tag_len_be, 2) <= 0) return false;
  if (recvall(cfd, &w_be, 4) <= 0) return false;
  if (recvall(cfd, &h_be, 4) <= 0) return false;
  if (recvall(cfd, &ch, 1) <= 0) return false;
  if (recvall(cfd, &len_be, 4) <= 0) return false;
  uint16_t tag_len = ntohs(tag_len_be);
  uint32_t w = ntohl(w_be), h = ntohl(h_be), avatar_len = ntohl(len_be);

  if (tag_len > 1024) return false;
  if (w == 0 || h == 0 || w > MAX_W || h > MAX_H) return false;
  if (!(ch==1 || ch==3 || ch==4)) return false;
  if (avatar_len != w*h*ch) return false;

  char *tag_buf = (char*)malloc(tag_len+1); if (!tag_buf) return false;
  uint8_t *avatar = (uint8_t*)malloc(avatar_len); if (!avatar) { free(tag_buf); return false; }
  if (recvall(cfd, tag_buf, tag_len) <= 0) { free(tag_buf); free(avatar); return false; }
  tag_buf[tag_len] = '\0';
  if (recvall(cfd, avatar, avatar_len) <= 0) { free(tag_buf); free(avatar); return false; }

  pthread_mutex_lock(&g_lock);
  Player *p = ensure_player(peer_ip_net);
  if (!p) { pthread_mutex_unlock(&g_lock); free(tag_buf); free(avatar); return false; }

    // --- RESUME HERE ---

  size_t cpy = tag_len; if (cpy > MAX_TAG_LEN) cpy = MAX_TAG_LEN;
  memcpy(p->tag, tag_buf, cpy); p->tag[cpy] = '\0';
  set_player_avatar(p, avatar, w, h, ch);
  p->connected = true; p->last_seen = time(NULL);
  uint32_t player_id = p->player_id; int px = p->pos_x, py = p->pos_y;
  pthread_mutex_unlock(&g_lock);

  // ACK
  uint8_t ack[1+4+4+4]; ack[0] = OPC_ACK;
  uint32_t be_player_id = htonl(player_id);
  uint32_t be_x = htonl(px), be_y = htonl(py);
  memcpy(&ack[1], &be_player_id, 4);
  memcpy(&ack[5], &be_x, 4);
  memcpy(&ack[9], &be_y, 4);
  sendall(cfd, ack, sizeof ack);

  free(tag_buf); free(avatar);
  return true;
}

// ---------- Network thread ----------
typedef struct { int listen_fd; } NetArgs;

static void add_client_fd_locked(int fd) { if (g_client_count < MAX_CLIENTS) g_client_fds[g_client_count++] = fd; }
static void remove_client_index_locked(size_t idx) {
  if (idx >= g_client_count) return;
  g_client_fds[idx] = g_client_fds[g_client_count-1];
  g_client_count--;
}

static void* net_thread_main(void *arg_) {
  NetArgs *args = (NetArgs*)arg_;
  int lfd = args->listen_fd; free(args);

  while (g_running) {
    fd_set rfds; FD_ZERO(&rfds);
    FD_SET(lfd, &rfds); int maxfd = lfd;

    pthread_mutex_lock(&g_lock);
    for (size_t i=0;i<g_client_count;i++) { FD_SET(g_client_fds[i], &rfds); if (g_client_fds[i] > maxfd) maxfd = g_client_fds[i]; }
    pthread_mutex_unlock(&g_lock);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 }; // 200ms tick
    int rv = select(maxfd+1, &rfds, NULL, NULL, &tv);
    if (rv < 0) { if (errno == EINTR) continue; perror("select"); break; }

    if (FD_ISSET(lfd, &rfds)) {
      struct sockaddr_in cli; socklen_t clilen = sizeof cli;
      int cfd = accept(lfd, (struct sockaddr*)&cli, &clilen);
      if (cfd >= 0) {
        pthread_mutex_lock(&g_lock);
        add_client_fd_locked(cfd);
        pthread_mutex_unlock(&g_lock);
      }
    }

    // What the fuck is all this

    pthread_mutex_lock(&g_lock);
    for (size_t i=0; i<g_client_count; ) {
      int cfd = g_client_fds[i]; bool advance = true;
      if (FD_ISSET(cfd, &rfds)) {
        struct sockaddr_in peer; socklen_t pl = sizeof peer;
        if (getpeername(cfd, (struct sockaddr*)&peer, &pl) < 0) { goto drop_locked; }
        uint32_t ip_net = peer.sin_addr.s_addr;

        pthread_mutex_unlock(&g_lock); // do blocking I/O unlocked
        bool ok = handle_register(cfd, ip_net);
        if (!ok) {
          pthread_mutex_lock(&g_lock);
          Player *p = find_player_by_ip(ip_net);
          if (p) p->connected = false;
          goto drop_locked;
        } else {
          // keep connection open; optionally peek for closure
          char tmp; ssize_t n = recv(cfd, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
          pthread_mutex_lock(&g_lock);
          if (n == 0) goto drop_locked;
        }
        goto next_locked;

      drop_locked:
        close(cfd);
        remove_client_index_locked(i);
        advance = false;
      next_locked:
        ;
      }
      if (advance) i++;
    }
    pthread_mutex_unlock(&g_lock);
  }

  // graceful: notify remaining clients and close
  pthread_mutex_lock(&g_lock);
  uint8_t bye = OPC_SHUTDOWN;
  for (size_t i=0;i<g_client_count;i++) { sendall(g_client_fds[i], &bye, 1); close(g_client_fds[i]); }
  g_client_count = 0;
  pthread_mutex_unlock(&g_lock);

  return NULL;
}

// -------------- RESUME HERE !!! ------------------

// ---------- Listener socket setup ----------
static int make_listener(const char *bind_ip, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) { perror("socket"); return -1; }
  int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in addr = {0}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) { fprintf(stderr, "bad bind ip\n"); close(fd); return -1; }
  if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0) { perror("bind"); close(fd); return -1; }
  if (listen(fd, 32) < 0) { perror("listen"); close(fd); return -1; }
  return fd;
}

// ---------- raylib helpers ----------
static void upload_texture_if_needed(Player *p) {
  if (!p->tex_dirty || !p->avatar) return;
  Image img = { .data = p->avatar, .width = (int)p->w, .height = (int)p->h, .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
  if (p->tex_inited) { UpdateTexture(p->tex, img.data); }
  else { p->tex = LoadTextureFromImage(img); p->tex_inited = true; }
  p->tex_dirty = false;
}

static void render_scene(void) {
  BeginDrawing();
  ClearBackground((Color){12,16,24,255});

  pthread_mutex_lock(&g_lock);
  for (size_t i=0;i<g_player_count;i++) {
    Player *p = &g_players[i];
    upload_texture_if_needed(p);
    if (p->connected && p->tex_inited) {
      Rectangle src = {0,0,(float)p->w,(float)p->h};
      float scale = 3.0f;
      Rectangle dst = {(float)p->pos_x, (float)p->pos_y, p->w*scale, p->h*scale};
      DrawTexturePro(p->tex, src, dst, (Vector2){0,0}, 0.0f, WHITE);
      // TODO: draw your tag above the avatar (using your own font/assets)
      // e.g., DrawTextEx(myFont, p->tag, (Vector2){dst.x, dst.y-18}, fontSize, spacing, WHITE);
    }
  }
  pthread_mutex_unlock(&g_lock);

  EndDrawing();
}

// ---------- Main ----------
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <bind_ip> <port>\n", argv[0]); return 1; }
  const char *bind_ip = argv[1]; uint16_t port = (uint16_t)atoi(argv[2]);

  srand((unsigned)time(NULL));

  // Init raylib window
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
  InitWindow(WINDOW_W, WINDOW_H, "Avatar Wall (raylib)");
  SetTargetFPS(60);

  int lfd = make_listener(bind_ip, port); if (lfd < 0) return 1;

  // Start network thread
  pthread_t net_thr; NetArgs *na = (NetArgs*)malloc(sizeof *na); na->listen_fd = lfd;
  if (pthread_create(&net_thr, NULL, net_thread_main, na) != 0) { perror("pthread_create"); return 1; }

  // Main render loop
  while (g_running && !WindowShouldClose()) {
    if (IsKeyPressed(KEY_ESCAPE)) g_running = false;
    render_scene();
  }

  // signal stop and join
  g_running = false;
  pthread_join(net_thr, NULL);
  close(lfd);

  // Cleanup GPU resources
  pthread_mutex_lock(&g_lock);
  for (size_t i=0;i<g_player_count;i++) {
    if (g_players[i].tex_inited) UnloadTexture(g_players[i].tex);
    free(g_players[i].avatar);
  }
  pthread_mutex_unlock(&g_lock);

  CloseWindow();
  return 0;
}
```
