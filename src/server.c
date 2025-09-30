// ==============================================================================
// INCLUDES
// ==============================================================================

#include "w-helper.h"
#include <pthread.h>
#include <raylib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// ==============================================================================
// CONFIGURATION
// ==============================================================================

#define MAX_CLIENTS 32          // Not too many!
#define MAX_PLAYERS MAX_CLIENTS // Alias for readability
#define MAX_TAG_LEN 31
#define MAX_AVATAR_W 8 // We expect an 8x8 sprite
#define MAX_AVATAR_H 8
#define AVATAR_CHANNEL_COUNT 4 // We always want avatars to end up as RGBA
#define RGBA_CHANNEL_COUNT 4   // Amnt. of channels in an RGBA image
#define RGB_CHANNEL_COUNT 3    // Amnt. of channels in an RGB image, no alpha data
#define WINDOW_W 500
#define WINDOW_H 500

// FIXME: Boundscheck these!
#define MIN_PLAYER_X_POS 100
#define MAX_PLAYER_X_POS 400
#define MIN_PLAYER_Y_POS 100
#define MAX_PLAYER_Y_POS 400

// ==============================================================================
// OUR PROTOCOL
// ==============================================================================

enum
{
  OPC_REGISTER = 0x01,
  OPC_ACK      = 0x81,
  OPC_SHUTDOWN = 0xFF
};

// ==============================================================================
// PLAYER DATA STRUCTURE
// ==============================================================================

typedef struct Player
{
  uint32_t ip;
  uint32_t player_id;
  char     tag[MAX_TAG_LEN + 1]; // +1 for null terminator
  int      pos_x, pos_y;
  uint8_t *avatar;   // Byte array containing image pixels (RGBA32)
  uint32_t w, h, ch; // Width, height and channel count
  bool     connected;
  /* time_t last_seen; // Not sure what this is for */
  Texture2D tex; // Created from avatar
  bool      tex_inited,
      tex_dirty; // Has avatar texture been initialized? / Has avatar texture been modified since initialization?
} Player;

// ==============================================================================
// GLOBAL SHARED STATE
// ==============================================================================

// TODO: I don't want to keep this here; might want to turn into externs
// Because I've never used that before, though, defer to when everything else is done!

static Player   g_players[MAX_PLAYERS]; // Player objects
static size_t   g_player_count = 0;
static int      g_client_fds[MAX_CLIENTS]; // Open connection file descriptors
static size_t   g_client_count = 0;
static uint32_t g_next_player_id =
    1; // Counter used to keep track of player IDs; we assign these incrementally as new players come in

// Thread locking system
static pthread_mutex_t g_lock    = PTHREAD_MUTEX_INITIALIZER;
static volatile bool   g_running = true; // Is server running?

// ==============================================================================
// PLAYER TABLE OPERATIONS
// ==============================================================================

/**
 * @brief Linear-search for a player by IP the players table
 * @param target_ip The IP of the player we wish to find
 * @returns The address of the player if found; NULL if did not find
 */
static Player *find_player_by_ip(uint32_t target_ip)
{
  for (size_t i = 0; i < g_player_count; ++i)
  {
    if (g_players[i].ip == target_ip)
    {
      return &g_players[i];
    }
  }

  return NULL;
}

/**
 * @brief Ensure that the player registered under target_ip exists; if not, create an entry for a player bound to target_ip
 * @param target_ip The player's IP
 * @returns The player's address if found already, NULL if there is no room for a new player, or the new player's address if a new one had to be created
 */
static Player *ensure_player(uint32_t target_ip)
{

  Player *p = find_player_by_ip(target_ip);

  // If the player is in the table, they exist; we #stillguhd
  if (p)
  {
    return p;
  }

  // If the player does NOT exist, we wanna make sure they do
  // Check that there is room for a new player

  if (g_player_count >= MAX_PLAYERS)
  {
    return NULL;
  }

  // Add the new player
  size_t  new_player_index = g_player_count + 1;
  Player *new_player       = &g_players[new_player_index];

  // Ensure memory spot for new player is clean!
  memset(new_player, 0, sizeof(*new_player));

  // Set their attributes
  new_player->ip = target_ip;

  uint32_t new_player_id = g_next_player_id + 1;
  new_player->player_id  = new_player_id;

  // Assign random pos
  new_player->pos_x = irand(MIN_PLAYER_X_POS, WINDOW_W - MAX_PLAYER_X_POS);
  new_player->pos_y = irand(MIN_PLAYER_Y_POS, WINDOW_H - MAX_PLAYER_Y_POS);

  new_player->connected  = false;
  new_player->tex_inited = false;
  new_player->tex_dirty  = false;

  return new_player;
}

/**
 * @brief Sets the player avatar image
 * @note The input image will always be converted to RGBA
 * @param Player Address of the player whose avatar we wish to set
 * @param av_pixels Byte array containing raw pixel data of new avatar
 * @param av_w Width of new avatar
 * @param av_h Height of new avatar
 * @param av_ch Channel count of new avatar
 * @returns true if player avatar was set, false otherwise
 */
static bool set_player_avatar(Player        *target_player,
                              const uint8_t *av_pixels,
                              uint32_t       av_w,
                              uint32_t       av_h,
                              uint8_t        av_ch)
{
  // Check the received avatar dimensions; if they exceed our maximums, we truncate
  // This is sensible and avoids crashing and dying and failing horribly

  if (av_w > MAX_AVATAR_W)
  {
    av_w = MAX_AVATAR_W;
  }

  if (av_h > MAX_AVATAR_H)
  {
    av_h = MAX_AVATAR_H;
  }

  // Allocate space for an RGBA image of the width and height we want
  // Regardless of the supplied channel count, we always want to convert into our expected channel count

  size_t image_buf_len =
      (size_t)av_w * av_h * RGBA_CHANNEL_COUNT; // Width * height * n. of channels we want
  uint8_t *image_buf = (uint8_t *)malloc(image_buf_len);

  if (!image_buf)
  {
    return false;
  }

  // Write avatar pixel values to allocated image buffer
  // We expect avatar to be RGBA; as such, we will convert as needed

  for (uint32_t y = 0; y < av_h; ++y)
  {
    for (uint32_t x = 0; x < av_w; ++x)
    {
      // Get the flat index of the xth pixel at the current column
      size_t i = (size_t)y * av_w + x;

      // Compute its R, G, B, and A
      uint8_t r, g, b, a;

      // RGBA to RGBA; basically no conversion we just pull all values as-is
      if (av_ch == RGBA_CHANNEL_COUNT)
      {
        r = av_pixels[i * RGBA_CHANNEL_COUNT + 0];
        g = av_pixels[i * RGBA_CHANNEL_COUNT + 1];
        b = av_pixels[i * RGBA_CHANNEL_COUNT + 2];
        a = av_pixels[i * RGBA_CHANNEL_COUNT + 3];
      }

      // RGB to RGBA; grab all RGB values and then just assume alpha is always max
      else if (av_ch == RGB_CHANNEL_COUNT)
      {
        r = av_pixels[i * RGB_CHANNEL_COUNT + 0];
        g = av_pixels[i * RGB_CHANNEL_COUNT + 1];
        b = av_pixels[i * RGB_CHANNEL_COUNT + 2];
        a = 255; // Since we're converting no alpha to alpha, we just set max alpha for all pixels
      }

      // Grayscale to RGBA; just grab the single pixel value's brightness (0-255); assign it to all channels, and assume alpha is max
      else
      {
        r = g = b = av_pixels[i];
        a         = 255;
      }

      // Write pixels to image buffer
      image_buf[i * RGBA_CHANNEL_COUNT + 0] = r;
      image_buf[i * RGBA_CHANNEL_COUNT + 1] = g;
      image_buf[i * RGBA_CHANNEL_COUNT + 2] = b;
      image_buf[i * RGBA_CHANNEL_COUNT + 3] = a;
    }
  }

  free(target_player->avatar);

  target_player->avatar = image_buf;
  target_player->w      = av_w;
  target_player->h      = av_h;
  target_player->ch     = RGBA_CHANNEL_COUNT; // Avatar is always RGBA!
  target_player->tex_dirty =
      true; // We have now modified the texture; WARNING: Shouldn't this also modify tex_inited? Isn't this where we initialize the texture?

  return true;
}

// ==============================================================================
// CLIENT HANDLING
// ==============================================================================

// NOTE: Resume here!
static bool handle_register(int cfd, uint32_t peer_ip_net);

// ==============================================================================
// NETWORK THREAD
// ==============================================================================

typedef struct
{
  int listen_fd;
} NetArgs; // FIXME: This is probably not necessary

static void  add_client_fd_locked(int fd);
static void  remove_client_index_locked(size_t idx);
static void *net_thread_main(void *arg_);

// ==============================================================================
// LISTENER HANDLING
// ==============================================================================

static int make_listener(const char *bind_ip, uint16_t port);

// ==============================================================================
// RAYLIB HELPER
// ==============================================================================

static void upload_texture_if_needed(
    Player *
        p); // FIXME: Is this even necessary? If we get a new texture from client, maybe...

static void render_scene();

// ==============================================================================
// MAIN LOOP
// ==============================================================================

int main(int argc, char *argv[]) { return 0; }
