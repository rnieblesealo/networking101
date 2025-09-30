# KH Networking 101 Workshop

This is a bunch of networking code I wrote for a workshop I will (hopefully) teach on network programming fundamentals. Enjoy!

## Activity/Workshop Architecture

- We program a basic client-server on our own; supports a single send and recv; CLI-only
- Then we program a special client that:
    - Connects to a server I will write (it is more complex)
    - Sends an image avatar to the server over a TCP socket (will be likely 16x16, very small)
    - Sends a gamertag to the server attached to image
    - Server will:
        - Store image, tag inside some kind of list 
        - Using SDL, will draw the image with the nametag above for that specific client's connection; a random position will be asigned to this "player"
        - Once the client closes the connection, the server will stop rendering the player, but their info will be kept in case they reconnect
        - If the client reopens the connection, they will be respawned at the same pos. (meaning their randomly assigned position will be mapped to their IP)
    - If the server is killed, all clients will be informed and they will terminate 
    - The clients will be CLI-only for avoiding complexity (we will write from top to bottom), but the server does have a window since I'll have it prewritten

## Things I Learned While Building This

> **Note:** The code I wrote is littered with comments! Check it out to see an in-depth into my thought process :)

- For raw networking stuff, it's useful to define our own protocol!
- If sending images over as raw pixels, we also need to include channel count, width, and height
- Heartbeats to check for alive connetion!
- `static` variables in C are kept in the static memory segment
- `volatile` = "A thread can randomly mess with this; don't optimize it to avoid bad behavior"
- `ssize_t` = Signed `size_t` which allows us to store -1 for error codes
- `extern` Allows sharing static variables between files
    - Declare as `extern` in header
    - Declare normally in `.c`
    - Include header
    - Use!
- All-or-error semantics = Either return a success value or an error code
- Partial progress semantics = Either return a success value or a partial progress value, which can potentially be an errcode
