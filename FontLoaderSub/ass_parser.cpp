#include "ass_parser.h"

typedef enum {
  PST_UNKNOWN = 0,
  // PST_INFO,
  PST_STYLES,
  PST_EVENTS,
  // PST_FONTS
} ASS_ParserState;

typedef enum {
  TRACK_TYPE_UNKNOWN = 0,
  TRACK_TYPE_ASS,
  TRACK_TYPE_SSA
} ASS_TrackType;

typedef struct {
  const char *begin;
  const char *end;
} ASS_U8Range;

typedef struct {
  ASS_U8Range Text;
} ASS_Event;

typedef struct {
  ASS_ParserState state;
  ASS_TrackType track_type;
  ASS_U8Range format_string;

  ASS_FontCallback callback;
  void *cb_arg;
} ASS_Track;

static int ass8_is_space(int ch) {
  return ch == ' ' || ch == '\t';
}

static void ass8_trim(ASS_U8Range *r) {
  if (!r || !r->begin || !r->end || r->begin == r->end)
    return;
  for (; r->begin != r->end && ass8_is_space(*r->begin); r->begin++) {
    // nop;
  }
  if (r->begin == r->end)
    return;
  for (; ass8_is_space(r->end[-1]); r->end--) {
    // nop;
  }
}

static const char *ass8_skip_spaces(const char *p, const char *end) {
  for (; p != end && ass8_is_space(*p); p++) {
    // nop
  }
  return p;
}

static int ass8_is_eol(int ch) {
  return ch == '\r' || ch == '\n';
}

static int ass8_strncmp(const char *s1, const char *s2, size_t cch) {
  char a, b;
  const char *last = s2 + cch;

  do {
    a = *s1++;
    b = *s2++;
  } while (s2 != last && a && a == b);

  return a - b;
}

static char ass8_to_lower(char ch) {
  if ('A' <= ch && ch <= 'Z')
    return ch - 'A' + 'a';
  return ch;
}

static int ass8_strncasecmp(const char *s1, const char *s2, size_t cch) {
  char a, b;
  const char *last = s2 + cch;

  do {
    a = ass8_to_lower(*s1++);
    b = ass8_to_lower(*s2++);
  } while (s2 != last && a && a == b);

  return a - b;
}

static const char *ass8_strnchr(const char *s, char ch, size_t cch) {
  const char *last = s + cch;

  for (; s != last && *s != ch; s++) {
    // nop
  }
  return s == last ? nullptr : s;
}

static void fire_font_cb(ASS_Track *track, ASS_U8Range *font) {
  if (track->callback) {
    const char *begin = ass8_skip_spaces(font->begin, font->end);
    track->callback(begin, font->end - begin, track->cb_arg);
  }
}

static int next_tok(ASS_U8Range *input, ASS_U8Range *tok) {
  if (input->begin == input->end) {
    return 0;
  }
  tok->begin = input->begin;
  tok->end = input->begin;
  while (tok->end != input->end && tok->end[0] != ',') {
    ++tok->end;
  }
  if (tok->end[0] == ',') {
    input->begin = tok->end + 1;
  } else {
    input->begin = tok->end;
  }
  ass8_trim(tok);

  return 1;
}

static int test_tag(
    const char *p,
    const char *end,
    const char *tag,
    size_t len,
    ASS_U8Range *arg) {
  if (end >= p + len && ass8_strncmp(p, tag, len) == 0) {
    arg->begin = p + len;
    arg->end = end;
    return 1;
  }
  return 0;
}

static const char *
parse_tags(ASS_Track *track, const char *p, const char *end, int nested) {
  const char *q;
  for (; p != end; p = q) {
    while (p != end && *p != '\\')
      ++p;
    if (*p != '\\')
      break;
    ++p;
    if (p != end)
      p = ass8_skip_spaces(p, end);

    q = p;
    while (q != end && *q != '(' && *q != '\\')
      ++q;
    if (q == p)
      continue;

    const char *name_end = q;

    // Split parenthesized arguments
    ASS_U8Range first_arg = {nullptr, nullptr};
    if (q != end && *q == '(') {
      ++q;
      while (1) {
        if (q != end)
          q = ass8_skip_spaces(q, end);
        const char *r = q;
        while (r != end && *r != ',' && *r != '\\' && *r != ')')
          ++r;

        if (r != end && *r == ',') {
          // push_arg(args, &argc, q, r);
          q = r + 1;
        } else {
          while (r != end && *r != ')')
            ++r;
          // push_arg(args, &argc, q, r);
          if (first_arg.begin == nullptr) {
            first_arg.begin = q;
            first_arg.end = r;
          }
          q = r;
          if (q != end)
            ++q;
          break;
        }
      }
    }

    ASS_U8Range arg;
    if (test_tag(p, name_end, "fn", 2, &arg)) {
      if (ass8_strncmp("0", arg.begin, arg.end - arg.begin) == 0) {
        // restore?
      } else {
        if (first_arg.begin)
          fire_font_cb(track, &first_arg);
        else
          fire_font_cb(track, &arg);
      }
    }
  }
  return p;
}

static void parse_events(ASS_Track *track, ASS_Event *event) {
  if (event->Text.begin == nullptr) {
    return;
  }

  const char *p = event->Text.begin;
  const char *ep = event->Text.end;
  const char *q;

  while ((p = ass8_strnchr(p, '{', ep - p)) != nullptr &&
         (q = ass8_strnchr(p, '}', ep - p)) != nullptr) {
    p = parse_tags(track, p, q, 0);
    ++p;
  }
}

static void
process_event_tail(ASS_Track *track, ASS_U8Range *line, int n_ignored) {
  int i;
  ASS_U8Range tok[1], tag[1], format[1];
  ASS_Event event = {};

  for (i = 0; i < n_ignored; i++) {
    next_tok(line, tok);
  }

  *format = track->format_string;
  if (format->begin == format->end) {
    // using fallback
    const int skips = 9;
    for (i = 0; i < skips; i++)
      next_tok(line, tok);
    if (next_tok(line, tok)) {
      tok->end = line->end;
      event.Text = *tok;
    }
  } else {
    while (next_tok(format, tag)) {
      const int r = next_tok(line, tok);
      if (r && tag->end - tag->begin == 4 &&
          ass8_strncasecmp(tag->begin, "text", 4) == 0) {
        // till the end
        tok->end = line->end;
        event.Text = *tok;
        break;
      }
    }
  }
  parse_events(track, &event);
}

static void
process_styles(ASS_Track *track, const char *begin, const char *end) {
  ASS_U8Range line[1], tok[1], tag[1], format[1];
  line->begin = begin;
  line->end = end;

  *format = track->format_string;
  if (format->begin == format->end) {
    // use default fallback, assuming fontname at column 2
    next_tok(line, tok);
    const int r = next_tok(line, tok);
    if (r) {
      fire_font_cb(track, tok);
    }
  } else {
    // using format string
    while (next_tok(format, tag)) {
      const int r = next_tok(line, tok);
      if (r && tag->end - tag->begin == 8 &&
          ass8_strncasecmp(tag->begin, "fontname", 8) == 0) {
        fire_font_cb(track, tok);
      }
    }
  }
}

static void
process_styles_line(ASS_Track *track, const char *begin, const char *end) {
  if (!ass8_strncmp(begin, "Format:", 7)) {
    track->format_string.begin = begin + 7;
    track->format_string.end = end;
    ass8_trim(&track->format_string);
  } else if (!ass8_strncmp(begin, "Style:", 6)) {
    process_styles(track, ass8_skip_spaces(begin + 6, end), end);
  }
}

static void
process_events_line(ASS_Track *track, const char *begin, const char *end) {
  if (!ass8_strncmp(begin, "Format:", 7)) {
    ASS_U8Range fmt_str = {begin + 7, end};
    track->format_string = fmt_str;
    ass8_trim(&track->format_string);
  } else if (!ass8_strncmp(begin, "Dialogue:", 9)) {
    ASS_U8Range range = {ass8_skip_spaces(begin + 9, end), end};
    process_event_tail(track, &range, 0);
  }
}

static void process_line(ASS_Track *track, const char *begin, const char *end) {
  int is_content = 0;

  if (!ass8_strncasecmp(begin, "[v4 styles]", 11)) {
    track->state = PST_STYLES;
    track->track_type = TRACK_TYPE_SSA;
  } else if (!ass8_strncasecmp(begin, "[v4+ styles]", 12)) {
    track->state = PST_STYLES;
    track->track_type = TRACK_TYPE_ASS;
  } else if (!ass8_strncasecmp(begin, "[events]", 8)) {
    track->state = PST_EVENTS;
  } else if (begin[0] == '[') {
    track->state = PST_UNKNOWN;
  } else {
    is_content = 1;
  }

  if (!is_content) {
    track->format_string.begin = nullptr;
    track->format_string.end = nullptr;
  } else {
    switch (track->state) {
    case PST_STYLES:
      process_styles_line(track, begin, end);
      break;
    case PST_EVENTS:
      process_events_line(track, begin, end);
      break;
    default:
      break;
    }
  }
}

void ass_process_data(
    const char *data,
    size_t cch,
    ASS_FontCallback cb,
    void *arg) {
  ASS_Track track = {};
  track.callback = cb;
  track.cb_arg = arg;
  const char *p = data;
  const char *eos = data + cch;
  while (p != eos) {
    // skip blank lines
    while (p != eos && ass8_is_eol(*p))
      ++p;
    // find end of the line
    const char *q = p;
    while (q != eos && !ass8_is_eol(*q))
      ++q;

    process_line(&track, p, q);
    p = q;
  }
}
