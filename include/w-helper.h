#ifndef S_STATE_H
#define S_STATE_H

#include <sys/types.h>

/**
 * @brief Return a random int between a and b, inclusive
 * @note Inclusivity comes from adding a; see implementation
 * @param a The left bound of random range
 * @param b The right bound of random range
 * @returns The random number
 */
int irand(int a, int b);

/**
 * @brief Receives exactly len bytes from a given connection
 * @note If necessary, we'll keep calling recv() until we meet the quota specified by len
 * @param fd The socket file desc. to receive from
 * @param buf The buffer to write the received data to
 * @param len The amount of bytes we wish to receive
 * @returns Amt. of successfully received bytes on success (== len), 0 if the peer was closed, or -1 if another failure ocurred
 */
ssize_t recvall(int fd, void *buf, size_t len);

/**
 * @brief Sends exactly len bytes from buf via a given connection
 * @note If necessary, we'll keep calling send() until all data goes through
 * @param fd The socket file desc. to send through
 * @param buf The source buffer containing the data we wish to send; const to avoid mods.
 * @param len The amount of bytes we wish to send; will most likely be sizeof buf
 * @returns Amt. of successfully sent bytes on success (== len), or -1 on failure
 */
ssize_t sendall(int fd, const void *buf, size_t len);

#endif
