#ifndef SSH_CHATTER_TRANSLATOR_H
#define SSH_CHATTER_TRANSLATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

void translator_global_init(void);
void translator_global_cleanup(void);

bool translator_translate(const char *text, const char *target_language,
                          char *translation, size_t translation_len,
                          char *detected_language, size_t detected_len);

bool translator_translate_with_cancel(const char *text,
                                      const char *target_language,
                                      char *translation, size_t translation_len,
                                      char *detected_language,
                                      size_t detected_len,
                                      const volatile bool *cancel_flag);

bool translator_eliza_respond(const char *prompt, char *reply,
                              size_t reply_len);

bool translator_moderate_text(const char *category, const char *content,
                              bool *blocked, char *reason, size_t reason_len);

const char *translator_last_error(void);

bool translator_last_error_was_quota(void);

void translator_set_gemini_enabled(bool enabled);
bool translator_is_gemini_enabled(void);
bool translator_is_gemini_manually_disabled(void);
bool translator_gemini_backoff_remaining(struct timespec *remaining);
void translator_clear_gemini_backoff(void);
bool translator_is_ollama_only(void);
void translator_set_manual_chat_bbs_only(bool enabled);
bool translator_is_manual_chat_bbs_only(void);
void translator_set_manual_skip_scrollback(bool enabled);
bool translator_is_manual_skip_scrollback(void);
bool translator_should_limit_to_chat_bbs(void);
bool translator_should_skip_scrollback_translation(void);

#endif
