#include "ol.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

olStr *olStr_make(char *data, u32 len) {
  if (len == 0) {
    len = strlen(data);
  }

  olStr *result = malloc(sizeof(olStr) + len + 1);
  result->len = len;
  memcpy_s(result->data, len, data, len);
  result->data[len] = 0;
  return result;
}

void olStr_delete(olStr *str) { free(str); }

olStrBuilder olStrBuilder_make(u32 reserve) {
  olStrBuilder result;
  if (reserve != 0) {
    result.cap = reserve;
  } else {
    result.cap = 10;
  }
  result.len = 0;
  result.data = (char *)malloc(result.cap);
  return result;
}

void olStrBuilder_appendc(olStrBuilder *sb, char c) {
  if (sb->len + 1 > sb->cap) {
    float mul = sb->cap > 512 ? 2 : 1.5;
    sb->cap *= mul;
    sb->data = (char *)realloc(sb->data, sb->cap);
  }
  sb->data[sb->len] = c;
  sb->len += 1;
}

void olStrBuilder_appends(olStrBuilder *sb, char *s, u32 len) {
  if (len == 0) {
    len = strlen(s);
  }

  u32 new_len = sb->len + len;
  while (new_len > sb->cap) {
    float mul = sb->cap > 512 ? 2 : 1.5;
    sb->cap *= mul;
    sb->data = (char *)realloc(sb->data, sb->cap);
  }
  memcpy_s(&sb->data[len], len, s, len);
  sb->len = new_len;
}

void olStrBuilder_reserve(olStrBuilder *sb, u32 additional_cap) {
  u32 new_cap = sb->len + additional_cap;
  if (new_cap > sb->cap) {
    sb->data = (char *)realloc(sb->data, new_cap);
    sb->cap = new_cap;
  }
}

olStr *olStrBuilder_finish(olStrBuilder *sb) {
  olStr *result = olStr_make(sb->data, sb->len);
  free(sb->data);
  return result;
}

void olStrBuilder_delete(olStrBuilder *sb) { free(sb->data); }

olStrView olStrView_strip(olStrView sv) {
  char *cur = sv.start;
  u32 new_len = sv.len;
  while (new_len > 0 && (*cur == ' ' || *cur == '\t')) {
    cur++;
    new_len--;
  }
  // skip to end
  char *end = advance_ptr(sv.start, sv.len);
  while (new_len > 0 && (*end == ' ' || *end == '\t')) {
    end--;
    new_len--;
  }
  return olStrView_make(cur, new_len);
}

olStrView olStrView_strips(olStr *str) {
  char *cur = str->data;
  u32 new_len = str->len;
  while (new_len > 0 && (*cur == ' ' || *cur == '\t')) {
    cur++;
    new_len--;
  }
  // skip to end
  char *end = advance_ptr((char *)str->data, str->len);
  while (new_len > 0 && (*end == ' ' || *end == '\t')) {
    end--;
    new_len--;
  }
  return olStrView_make(cur, new_len);
}

// SECTION: ARENA
olArena olArena_make(u64 size) {
  olArena result;
  result.last_section = 0;
  result.last_alloc_size = 0;
  result.cap = size;
  if (size == 0) {
    result.cap = 1024 * 4 * 4; // 16 kiB
  }
  result.data = malloc(result.cap);
  result.cur = result.data;
  return result;
}

// TODO: what to do when arena is full?
void *olArena_alloc(olArena *arena, u64 size) {
  void *new = advance_ptr(arena->cur, size);
  void *end = advance_ptr(arena->cur, arena->cap);
  if ((u64) new > (u64)end) {
    return null;
  }
  void *result = arena->cur;
  arena->cur = new;
  arena->last_alloc_size = size;
  return result;
}

void olArena_free_last(olArena *arena) {
  if (arena->last_alloc_size > 0) {
    advance_ptr(arena->data, -arena->last_alloc_size);
  }
}

void olArena_reset(olArena *arena) {
  arena->last_alloc_size = 0;
  arena->last_section = null;
  arena->cur = arena->data;
}

void olArena_push_section(olArena *arena) {
  void **section_loc = (void **)olArena_alloc(arena, sizeof(void *));
  *section_loc = arena->last_section;
  arena->last_section = section_loc;
}

void olArena_pop_section(olArena *arena) {
  if (arena->last_section == null) {
    return;
  }
  arena->cur = arena->last_section;
  arena->last_section = *(void **)arena->cur;
}

// SECTION LIST
olArray olArray_make(u32 element_size, u32 element_count) {
  olArray result;
  if (element_count == 0) {
    element_count = 10;
  }
  result.cap = element_count;
  result.used = 0;
  result.data = malloc(element_size * result.cap);
  result.element_size = element_size;
  return result;
}

void *olArray_push(olArray *list) {
  list->used += 1;
  if (list->used > list->cap) {
    float mul = list->used > 100 ? 1.5 : 2.0;
    list->cap *= mul;
    list->data = realloc(list->data, list->cap);
  }
  return advance_ptr(list->data, (list->used - 1) * list->element_size);
}

void *_olArray_pop(olArray *list) {
  void *result = olArray_getlast(list);
  list->used -= 1;
  return result;
}

void *olArray_get(olArray *list, u32 index) {
  if (index >= list->used) {
    return null;
  }
  void *result = advance_ptr(list->data, index * list->element_size);
  return result;
}

void *olArray_getlast(olArray *list) {
  return advance_ptr(list->data, (list->used - 1) * list->element_size);
}

void olArray_delete(olArray *list) { free(list->data); }

// SECTION LINKEDLIST
olList olList_make(u32 element_size) {
  olList result;
  result.element_size = element_size;
  result.cap = 10;
  result.len = 0;
  result.last_node = 0;
  result.next = malloc(sizeof(u32) * result.len);
  result.prev = malloc(sizeof(u32) * result.len);
  result.values = malloc(element_size * result.len);
  return result;
}

void *olList_push(olList *ll) {
  if (ll->len + 1 > ll->cap) {
    float mul = ll->len > 128 ? 1.5 : 2.0;
    ll->cap *= mul;
    ll->next = realloc(ll->next, ll->cap);
    ll->prev = realloc(ll->prev, ll->cap);
    ll->values = realloc(ll->values, ll->cap);
  }
  olListNode new_node = ll->len;
  ll->len += 1;
  ll->prev[new_node] = ll->last_node;
  ll->next[new_node] = UINT32_MAX;
  ll->next[ll->last_node] = ll->len;
  ll->last_node = new_node;
  void *result = &ll->values[new_node];
  return result;
}

void olList_pop(olList *ll) {
  olListNode last_node = ll->last_node;
  ll->last_node = ll->prev[last_node];
  ll->prev[last_node] = UINT32_MAX;
  ll->next[last_node] = UINT32_MAX;
}

olListNode olList_prev(olList *ll, olListNode n) { return ll->prev[n]; }

olListNode olList_next(olList *ll, olListNode n) { return ll->next[n]; }

void *olList_val(olList *ll, olListNode n) {
  return ((void **)(ll->values))[n];
}

void olList_remove(olList *ll, olListNode node) {
  olListNode prev = ll->prev[node];
  olListNode next = ll->next[node];
  ll->next[prev] = next;
  ll->prev[next] = prev;
}

void olList_delete(olList *ll) {
  free(ll->next);
  free(ll->prev);
  free(ll->values);
}

// SECTION MAP
olMap olMap_make(u32 element_size) {
  olMap result;
  result.cap = 2*2*2*2;
  result.used = 0;
  result.element_size = sizeof(u64) + element_size;
  result.data = malloc(result.element_size * result.cap);
  memset(result.data, 0, result.cap * result.element_size);
  return result;
}

u64 fnv1a(olStr *str) {
  const u64 magic_prime = 0x00000100000001b3;
  u64 hash = 0xcbf29ce484222325;
  char *cur = str->data;
  char *end = advance_ptr((char *)str->data, str->len);
  for (; cur < end; cur++) {
    hash = (hash ^ *cur) * magic_prime;
  }
  return hash;
}

static olMapSlot* get_slot(olMap* map, u32 slot_index) {
    return advance_ptr(map->data, slot_index * map->element_size);
}

void *olMap_inserth(olMap *map, u64 hash) {
  u64 new_cap = (map->used + 1) / 0.75;
  if (new_cap > map->cap) {
    olMapSlot *old_data = map->data;
    u32 old_cap = map->cap;
    map->cap *= 2;
    map->data = malloc(map->cap * map->element_size);
    memset(map->data, 0, map->cap * map->element_size);
    // rehashing
    olMapSlot *end = advance_ptr(old_data, old_cap * map->element_size);
    olMapSlot *cur = old_data;
    while (cur < end) {
      if (cur->hash == 0) {
        advance_ptr(cur, map->element_size);
        continue;
      }
      olMap_inserth(map, cur->hash);
      advance_ptr(cur, map->element_size);
    }
    free(old_data);
  }
  map->used += 1;
  u32 slot_index = hash & (map->cap - 1);
  olMapSlot *slot = get_slot(map, slot_index);
  while (slot->hash != 0 && slot->hash != hash) {
    slot_index = (slot_index + 1) & (map->cap - 1);
    slot = get_slot(map, slot_index);
  }
  slot->hash = hash;
  return &slot->data;
}

void *olMap_geth(olMap *map, u64 hash) {
  u32 slot_index = hash & (map->cap - 1);
  olMapSlot *slot = get_slot(map, slot_index);
  while (slot->hash != hash) {
    if (slot->hash == 0) {
      // key not found
      return null;
    }
    slot_index = (slot_index + 1) & (map->cap - 1);
    slot = get_slot(map, slot_index);
  }
  return slot->data;
}

void *olMap_get(olMap *map, olStr *key) {
  u64 hash = fnv1a(key);
  return olMap_geth(map, hash);
}

void *olMap_insert(olMap *map, olStr *key) {
  u64 hash = fnv1a(key);
  return olMap_inserth(map, hash);
}

void olMap_remove(olMap *map, olStr *key) {
  u64 hash = fnv1a(key);
  u32 slot_index = hash & (map->cap - 1);
  olMapSlot *slot = get_slot(map, slot_index);
  while (slot->hash != hash) {
    slot_index = (slot_index + 1) & (map->cap - 1);
    slot = get_slot(map, slot_index);
  }
  slot->hash = 0;
}

void *olMap_insertc(olMap *map, char *key, u32 len) {
  olStr *str = olStr_make(key, len);
  return olMap_insert(map, str);
}

void olMap_removec(olMap *map, char *key, u32 len) {
  olStr *str = olStr_make(key, len);
  return olMap_remove(map, str);
}

void *olMap_getc(olMap *map, char *key, u32 len) {
  olStr *str = olStr_make(key, len);
  return olMap_get(map, str);
}

void olMap_delete(olMap *map) { free(map->data); }

// SECTION CONSOLE
bool use_color = true;

const char *colors[] = {"\x1b[90m", "\x1b[32m", "\x1b[36m",
                        "\x1b[33m", "\x1b[31m", "\x1b[91m"};

const char *log_levels[] = {
    " DEBUG ", " INFO ", " WARN ", " ERROR ", " FATAL ",
};

void olLog_set_color(colors_e color) {
  if (!use_color)
    return;
  printf("%s", colors[color]);
}

void olLog_set_bold() {
  if (!use_color)
    return;
  printf("\x1b[1m");
}

void olLog_reset_bold() {
  if (!use_color)
    return;
  printf("\x1b[22m");
}

void olLog_set_underline() {
  if (!use_color)
    return;
  printf("\x1b[4m");
}

void olLog_reset_underline() {
  if (!use_color)
    return;
  printf("\x1b[24m");
}

void olLog_reset(void) {
  if (!use_color)
    return;
  printf("\x1b[0m");
}

void olLog_print_time() {
  char buf[50];
  time_t raw_time;
  time(&raw_time);
  struct tm info;
  localtime_s(&info, &raw_time);
  strftime(buf, 49, "%X", &info);
  printf("%s", buf);
}

void _log_(char *file, int line, log_level_e level, char *fmt, ...) {
  olLog_set_color(COLOR_GREY);
  olLog_print_time();

  olLog_set_color(level + 1);
  olLog_set_bold();
  printf("%s", log_levels[level]);
  olLog_reset_bold();

  olLog_set_color(COLOR_GREY);
  printf("%s:%d ", file, line);
  olLog_reset();

  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);

  printf("\n");
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void olLog_init() {
  HANDLE hconsole = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hconsole == null) {
    return;
  }
  DWORD cur_mode;
  bool ok = GetConsoleMode(hconsole, &cur_mode);
  if (!ok) {
    printf("ERROR: failed to get console mode!\n");
    return;
  }

  ok = SetConsoleMode(hconsole, cur_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                    ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  if (!ok) {
    printf("ERROR: failed to set console mode!");
    return;
  }
  use_color = true;
}

#endif