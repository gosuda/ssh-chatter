# Output Buffering System

## Overview

The output buffering system prevents screen flickering by accumulating multiple output operations into a buffer and then flushing them all at once. This is particularly useful for complex screen renderings like games, BBS posts, and editors where multiple components need to be drawn.

## Problem

Without buffering, each output operation (`session_send_line`, `session_send_raw_text`, etc.) results in an immediate write to the SSH/Telnet channel. When rendering complex screens with many components, this causes:

1. **Flickering**: Screen updates appear incrementally, causing visible "tearing" effects
2. **Performance overhead**: Many small writes are less efficient than one large write
3. **Poor user experience**: Users can see partial screen states during rendering

## Solution

The buffering system allows output to be accumulated in memory and written as a single operation:

```c
// Without buffering (flickering)
session_send_line(ctx, "Line 1");  // Write 1
session_send_line(ctx, "Line 2");  // Write 2
session_send_line(ctx, "Line 3");  // Write 3

// With buffering (no flickering)
session_output_buffer_start(ctx);
session_send_line(ctx, "Line 1");  // Buffered
session_send_line(ctx, "Line 2");  // Buffered
session_send_line(ctx, "Line 3");  // Buffered
session_output_buffer_stop(ctx);   // Single write
```

## API

### `session_output_buffer_start(session_ctx_t *ctx)`
Enables buffering mode. All subsequent output operations will accumulate in the buffer instead of writing directly.

### `session_output_buffer_stop(session_ctx_t *ctx)`
Flushes the buffer and disables buffering mode. All accumulated output is written in a single operation.

### `session_output_buffer_flush(session_ctx_t *ctx)`
Flushes the buffer without disabling buffering mode. The buffer is cleared but buffering remains active.

### `session_output_buffer_clear(session_ctx_t *ctx)`
Clears the buffer without flushing. Accumulated output is discarded.

## Implementation Details

### Buffer Size
The buffer size is defined by `SSH_CHATTER_OUTPUT_BUFFER_SIZE` (65536 bytes). This is large enough to handle most screen renderings.

### Auto-Flush
If the buffer becomes full during accumulation, it automatically flushes and continues buffering. This prevents buffer overflows while maintaining the batching benefits.

### Large Writes
If a single write exceeds the buffer size, it bypasses the buffer and writes directly. This prevents infinite flush loops.

### Thread Safety
The buffering system respects existing output locks. When `session_channel_write` is called, it checks the buffering flag before acquiring locks.

## Usage Examples

### Game Rendering (Tetris)
```c
static void session_game_tetris_render(session_ctx_t *ctx)
{
    // Prepare screen data in local buffer
    char *buffer = ctx->tetris_screen_buffer;
    // ... build screen content ...
    
    // Send entire screen at once
    session_output_buffer_start(ctx);
    session_send_raw_text(ctx, buffer);
    session_output_buffer_stop(ctx);
}
```

### BBS Post Rendering
```c
static void session_bbs_render_post(session_ctx_t *ctx, const bbs_post_t *post)
{
    session_output_buffer_start(ctx);
    
    session_bbs_prepare_canvas(ctx);
    session_send_plain_line(ctx, title_line);
    session_send_plain_line(ctx, author_line);
    session_send_plain_line(ctx, created_line);
    session_send_plain_line(ctx, SSH_CHATTER_BBS_EDITOR_BODY_DIVIDER);
    session_send_raw_text(ctx, post->body);
    session_render_prompt(ctx, true);
    
    session_output_buffer_stop(ctx);
}
```

### Complex Editor
```c
static void session_bbs_render_editor(session_ctx_t *ctx, const char *status)
{
    session_output_buffer_start(ctx);
    
    // Clear screen and render all components
    session_bbs_prepare_canvas(ctx);
    session_send_plain_line(ctx, title_line);
    session_send_plain_line(ctx, tags_line);
    session_send_plain_line(ctx, SSH_CHATTER_BBS_EDITOR_BODY_DIVIDER);
    
    // Render all editor lines
    for (size_t idx = 0U; idx < line_count; ++idx) {
        session_send_plain_line(ctx, line_buffers[idx]);
    }
    
    session_send_plain_line(ctx, SSH_CHATTER_BBS_EDITOR_END_DIVIDER);
    session_send_plain_line(ctx, hints);
    session_render_prompt(ctx, false);
    
    session_output_buffer_stop(ctx);
}
```

## When to Use Buffering

### ✅ Good Use Cases
- **Full screen redraws**: Games, editors, BBS posts
- **Multi-component UIs**: Screens with headers, content, and footers
- **Animated content**: Rapid updates where flickering is noticeable
- **List rendering**: Long lists or tables

### ❌ Avoid Buffering For
- **Single line output**: No benefit from buffering
- **Interactive prompts**: Users expect immediate feedback
- **Real-time chat**: Messages should appear as they arrive
- **Error messages**: Should be visible immediately

## Performance Considerations

### Benefits
1. **Reduced system calls**: One write instead of many
2. **Network efficiency**: Better for SSH protocol framing
3. **CPU usage**: Less context switching between user/kernel space
4. **Latency hiding**: User sees complete screen, not partial updates

### Overhead
The buffering system has minimal overhead:
- Memory: 64KB per session (already allocated)
- CPU: One memcpy per write + one flush per rendering
- Latency: None (flush is immediate after rendering)

## Debugging

### Enable Logging
To debug buffering issues, add logging to the buffer functions:

```c
printf("[buffer] start buffering (ctx=%p)\n", (void *)ctx);
printf("[buffer] append %zu bytes (total=%zu)\n", length, ctx->output_buffer_length);
printf("[buffer] flush %zu bytes\n", ctx->output_buffer_length);
```

### Common Issues

**Problem**: Output not appearing
- **Cause**: Forgot to call `session_output_buffer_stop()`
- **Solution**: Always pair `start` with `stop`

**Problem**: Partial output visible
- **Cause**: Buffer overflow with large content
- **Solution**: Check buffer size or split into smaller renders

**Problem**: Flickering still occurs
- **Cause**: Some outputs bypass buffering
- **Solution**: Ensure all outputs in the render function are within the buffer scope

## Future Enhancements

Possible improvements to the buffering system:

1. **Dynamic buffer sizing**: Grow buffer as needed instead of fixed size
2. **Multiple buffers**: Allow nested buffering contexts
3. **Compression**: Compress buffer content before sending
4. **Metrics**: Track buffer usage and flush frequency
5. **Per-transport tuning**: Different buffer sizes for SSH vs Telnet

## Related Files

- `include/ssh_chatter/host.h`: Buffer field definitions
- `src/host_parts/host_eliza_and_storage.c`: Buffer implementation
- `src/host_parts/host_bbs_and_games.c`: Game rendering with buffering
- `src/host_parts/host_session_output.c`: BBS rendering with buffering
