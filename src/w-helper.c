#include "w-helper.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int irand(int a, int b)
{
  // WARNING: Might want to re-seed generator on main
  return a + rand() % (b - a + 1);
}

ssize_t recvall(int fd, void *buf, size_t len)
{
  uint8_t *curr  = (uint8_t *)buf; // Current location on buffer
  size_t   nrecv = 0;              // Amt. of received bytes out of len thus far

  while (nrecv < len)
  {
    // Shift the buffer ptr. by the amt. of bytes already sent over
    // Adjust the length to match!
    // This will happen on an unsuccessful contiguous recv.
    ssize_t nbytes = recv(fd, curr + nrecv, len - nrecv, 0);

    // If we get 0 bytes, the peer is closed
    if (nbytes == 0)
    {
      return 0;
    }

    if (nbytes < 0)
    {
      perror("server: recvall");

      // EINTR = An async function interrupted this call; looks like we may recover from this by simply redoing loop
      if (errno == EINTR)
      {
        printf("server: recvall: resuming\n");
        continue;
      }

      // Any other signal is deadly
      return -1;
    }

    // Advance amnt. received bytes
    nrecv += (size_t)nbytes;
  }

  return (ssize_t)nrecv;
}

ssize_t sendall(int fd, const void *buf, size_t len)
{
  const uint8_t *curr  = (const uint8_t *)buf;
  size_t         nsent = 0;

  while (nsent < len)
  {
    ssize_t nbytes = send(fd, curr + nsent, len - nsent, 0);

    // If sent 0 or we get negative, there is error
    // This differs from recv; receiving 0 here doesn't reliably tell us the peer is closed
    // i.e. We just handle it as a normal error
    if (nbytes <= 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      return -1; // Any other signal is deadly
    }

    nsent += (ssize_t)nbytes;
  }

  return (ssize_t)nsent;
}

// NOTE: We're using all-or-error semantics; we either return nsent/nrecv (which must be the length) or -1
