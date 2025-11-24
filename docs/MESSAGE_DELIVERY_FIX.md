# Message Delivery Fix

## Issue Description

Previously, messages from other users would not appear on a user's terminal immediately when sent. Users would only see messages from others AFTER they typed something themselves. This created a poor user experience where the chat appeared to be broken or laggy.

## Root Cause

The problem was caused by buffering in the libssh library:

1. When a user sends a message, `chat_room_broadcast()` sends the message to all other connected users
2. The message data is written to each user's SSH channel using `ssh_channel_write()`
3. However, `ssh_channel_write()` only writes to libssh's internal buffers - it doesn't force the data to be sent over the network
4. The data would sit in the buffer until the SSH event loop processed it
5. The SSH event loop typically processes pending data when the session reads input (i.e., when the user types something)
6. This caused messages to appear "stuck" until the receiving user typed something

## Solution

The fix adds explicit flushing of SSH channels after broadcasting messages:

### 1. New Function: `session_channel_flush()`

Located in `src/host_parts/host_eliza_and_storage.c`:

```c
static void session_channel_flush(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // For telnet, flush buffered output and nudge the socket so queued bytes
    // are delivered immediately even if the client is idle
    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        session_output_buffer_flush(ctx);
        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLOUT,
            .revents = 0,
        };
        if (poll(&pfd, 1, SSH_CHATTER_CHANNEL_WRITE_TIMEOUT_MS) > 0 &&
            (pfd.revents & POLLOUT) != 0) {
            send(ctx->telnet_fd, "", 0, MSG_NOSIGNAL);
        }
        return;
    }

    // For SSH, flush any pending data in libssh's buffers
    if (ctx->session != nullptr) {
        // Use a short timeout (50ms) to avoid blocking
        ssh_blocking_flush(ctx->session, 50);
    }
}
```

This function:
- Calls `ssh_blocking_flush()` from libssh to force buffered data to be transmitted
- Uses a 50ms timeout to avoid blocking indefinitely
- Also forces telnet sockets to flush any pending bytes so chat output stays
  synchronized even when the user is idle

### 2. Flush After Broadcasting

The flush is called after sending messages in three broadcast functions:

- `chat_room_broadcast()` - regular chat messages
- `chat_room_broadcast_caption()` - caption/translation messages
- `chat_room_broadcast_entry()` - history entry messages

Example from `chat_room_broadcast()`:

```c
session_send_plain_line(member, formatted);

// Flush the channel to ensure immediate delivery
session_channel_flush(member);

if (member->history_scroll_position == 0U) {
    session_refresh_input_line(member);
}
```

## Testing

To verify the fix works:

1. Start the ssh-chatter server
2. Connect with two or more SSH clients
3. Send a message from one client
4. The message should appear immediately on all other clients without them needing to type anything

## Performance Impact

The performance impact is minimal:
- The flush operation has a 50ms timeout, but typically completes much faster
- Flushing only happens when broadcasting messages (not on every write)
- The flush is necessary for proper real-time chat functionality

## Alternative Solutions Considered

1. **Reducing the read timeout**: This would make the loop wake up more frequently, but would increase CPU usage and still have latency
2. **Using SSH events/select**: This would require a significant architectural change
3. **Current solution**: Explicit flushing is simple, effective, and has minimal overhead
