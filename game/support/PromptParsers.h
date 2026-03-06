#ifndef PROMPT_PARSERS_H
#define PROMPT_PARSERS_H

#include <string>

std::string clean_response(const std::string& raw_reply);
int parse_target_from_reply(const std::string& raw_reply, int fallback = -1);
std::string parse_speech_from_reply(const std::string& raw_reply);
std::string parse_thought_from_reply(const std::string& raw_reply, const std::string& fallback = "");

#endif
