#include "ol.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

double olTimer_freq = 0;
void* _readbytes(u8** ptr, u32 size);

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

void olStrBuilder_appendf(olStrBuilder* sb, char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  u32 required_size = vsnprintf(null, 0, fmt, args);
  u32 new_len = sb->len + required_size;
  while (new_len > sb->cap) {
    float mul = sb->cap > 512 ? 2 : 1.5;
    sb->cap *= mul;
    sb->data = (char *)realloc(sb->data, sb->cap);
  }
  char* buf = advance_ptr(sb->data, sb->len);
  vsnprintf(buf, required_size, fmt, args);
  va_end(args);
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

olArray olArray_copy(olArray* list) {
  olArray result = olArray_make(list->element_size, list->cap);
  result.used = list->used;
  memcpy_s(result.data, list->cap * list->element_size, list->data, list->used * list->element_size);
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

void olArray_reserve(olArray* list, u32 additional) {
  if (list->used + additional > list->cap) {
    float mul = list->used > 100 ? 1.5 : 2.0;
    list->cap *= mul;
    list->data = realloc(list->data, list->cap);
  }
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

void olArray_remove(olArray* list, u32 index) {
  void* last = olArray_getlast(list);
  void* cur = olArray_get(list, index);
  memcpy_s(cur, list->element_size, last, list->element_size);
  list->used -= 1;
}

void *olArray_getlast(olArray *list) {
  return advance_ptr(list->data, (list->used - 1) * list->element_size);
}

void olArray_delete(olArray *list) { free(list->data); }

olArray olArray_concat(olArray* a, olArray* b) {
  if (a->element_size != b->element_size) return (olArray){.cap=0};
  olArray result = olArray_make(a->element_size, (a->used+b->used));
  memcpy_s(result.data, result.element_size*result.cap, a->data, a->element_size*a->used);
  void* start = advance_ptr(result.data, a->used * a->element_size);
  memcpy_s(start, result.element_size*b->used, b->data, b->used * result.element_size);
  result.used = a->used + b->used;
  return result;
}

u32 olArray_find(olArray* list, void* find) {
  for (int i = 0; i < list->used; i++) {
    void* v = olArray_get(list, i);
    if (memcmp(v, find, list->element_size)) {
      return i;
    }
  }
  return 0;
}

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

olMap olMap_copy(olMap* map) {
  olMap result;
  result.cap = map->cap;
  result.used = map->used;
  result.element_size = map->element_size;
  result.data = malloc(result.element_size * result.cap);
  memcpy_s(result.data, result.element_size * result.cap, map->data, result.element_size * result.cap);
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

void olMap_removeh(olMap *map, olHash key) {
  u32 slot_index = key & (map->cap - 1);
  olMapSlot *slot = get_slot(map, slot_index);
  while (slot->hash != key) {
    slot_index = (slot_index + 1) & (map->cap - 1);
    slot = get_slot(map, slot_index);
  }
  slot->hash = 0;
}

void olMap_remove(olMap* map, olStr* str) {
  olHash h = fnv1a(str);
  return olMap_removeh(map, h);
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

bool olMap_next(olMap* map, olMapSlot** cur) {
  if (*cur == null) {
    *cur = map->data;
    return true;
  }
  olMapSlot* end = advance_ptr(map->data, map->element_size*map->cap);
  *cur = advance_ptr(*cur, map->element_size);
  if (*cur < end) {
    return true;
  }
  return false;
}

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
  olLog_set_color(OLCOLOR_GREY);
  olLog_print_time();

  olLog_set_color(level + 1);
  olLog_set_bold();
  printf("%s", log_levels[level]);
  olLog_reset_bold();

  olLog_set_color(OLCOLOR_GREY);
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

void olTimer_init(void) {
  LARGE_INTEGER li;
  QueryPerformanceFrequency(&li);
  olTimer_freq = (double)li.QuadPart / 1000.0;
}

void olLog_init() {
  olTimer_init();
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

void ol_init(void) {
  olLog_init();
  olTimer_init();  
}

// SECTION SET
olSet olSet_make() {
  olSet result;
  result.cap = 4;
  result.used = 0;
  result.data = calloc(1, result.cap * sizeof(olHash));
  return result;
}

void olSet_inserth(olSet* s, olHash h) {
  u64 new_cap = (s->used + 1) / 0.75;
  if (new_cap > s->cap) {
    olHash* old_data = s->data;
    u32 old_cap = s->cap;
    s->cap *= 2;
    s->data = malloc(s->cap * sizeof(olHash));
    memset(s->data, 0, s->cap * sizeof(olHash));
    // rehashing
    olHash* end = advance_ptr(s->data, s->cap * sizeof(olHash));    
    olHash* cur = s->data;
    while (cur < end) {
      if (*cur != 0) {
        olSet_inserth(s, *cur);
      }
      advance_ptr(cur, sizeof(olHash));
    }
  }
  
  s->used += 1;
  u32 slot_index = h & (s->cap - 1);
  olHash* slot = advance_ptr(s->data, slot_index);
  while (*slot != 0 && *slot != h) {
    slot_index = (slot_index + 1) & (s->cap - 1);
    slot = advance_ptr(s->data, slot_index);
  }
  *slot = h;
}

void olSet_insert(olSet* s, olStr* str) {
  olHash h = fnv1a(str);
  return olSet_inserth(s, h);
}

void olSet_insertc(olSet* s, char* c, u32 len) {
  olStr* str = olStr_make(c, len);
  olHash h = fnv1a(str);
  return olSet_inserth(s, h);
}

void olSet_removeh(olSet* s, olHash h) {
  u32 slot_index = h & (s->cap - 1);
  olHash* slot = advance_ptr(s->data, slot_index);
  while (*slot != 0 && *slot != h) {
    slot_index = (slot_index + 1) & (s->cap - 1);
    slot = advance_ptr(s->data, slot_index);
  }
  *slot = 0;
}

void olSet_remove(olSet* s, olStr* str) {
  olHash h = fnv1a(str);
  return olSet_inserth(s, h);
}

void olSet_removec(olSet* s, char* c, u32 len) {
  olStr* str = olStr_make(c, len);
  olHash h = fnv1a(str);
  return olSet_inserth(s, h);
}

bool olSet_findh(olSet* s, olHash h) {
  u64 slot_index = h & (s->cap - 1);
  olHash* slot = advance_ptr(s->data, slot_index * sizeof(olHash));
  while (*slot != 0) {
    if (*slot == h) return true;
    slot_index = (slot_index + 1) & (s->cap - 1);
    slot = advance_ptr(s->data, slot_index);
  }
  return false;
}

bool olSet_find(olSet* s, olStr* str) {
  olHash h = fnv1a(str);
  return olSet_findh(s, h);
}

bool olSet_findc(olSet* s, char* c, u32 len) {
  olStr* str = olStr_make(c, len);
  olHash h = fnv1a(str);
  return olSet_findh(s, h);
}

u64 next_pow2(u64 x) {
  return x == 1 ? 1 : (1 << (64 - __builtin_clzl(x-1)));
}

bool olSet_next(olSet* a, olHash** last) {
  if (*last == null) {
    *last = a->data;
  }
  olHash* end = advance_ptr(a->data, a->cap);
  *last = advance_ptr(*last, sizeof(olHash));
  if (*last < end) {
    return true;
  }
  return false;
}

olSet olSet_union(olSet* a, olSet* b) {
  u64 sum_used = a->used + b->used;
  u64 new_cap = next_pow2(sum_used);
  olSet result = (olSet) {
    .cap = new_cap, .used = 0,
    .data = calloc(1, new_cap * sizeof(olHash))
  };
  // insert a's hashes
  olHash* cur = null;
  while (olSet_next(a, &cur)) {
    olSet_inserth(&result, *cur);
  }
  // insert b's hashes
  cur = null;
  while (olSet_next(b, &cur)) {
    olSet_inserth(&result, *cur);
  }
  return result;
}

olSet olSet_diff(olSet* a, olSet* b);

olSet olSet_intersect(olSet* a, olSet* b) {
  olSet* smaller = a->used < b->used ? a : b;
  olSet* bigger = smaller == a ? b : a;

  olSet result = (olSet) {
                   .cap = smaller->cap,
                   .used = 0,
                   .data = calloc(1, sizeof(olHash) * smaller->cap)
                 };
  
  olHash* h = null;
  while (olSet_next(smaller, &h)) {
    if (olSet_findh(bigger, *h)) { // hash exist in both sets
      olSet_inserth(&result, *h);
    }
  }
  return result;
}

bool olSet_issubset(olSet* a, olSet* b) {
  olSet* smaller = a->used < b->used ? a : b;
  olSet* bigger = smaller == a ? b : a;
  olHash* h = null;
  while (olSet_next(smaller, &h)) {
    if (!olSet_findh(bigger, *h)) {
      return false;
    }
  }
  return true;
}

// SECTION TIMING
i64 olTimer_get() {
#ifdef _WIN32
  LARGE_INTEGER ticks;
  QueryPerformanceCounter(&ticks);
  return ticks.QuadPart;
#endif
}

double olTimer_getms(i64 start, i64 end) {
  return (end-start) / olTimer_freq;
}

// SECTION IMAGE
olGrayscaleImage olImage_to_grayscale(olImage* img) {
  olGrayscaleImage result;
  OLTIME("to grayscale") {
  const u8 rfac = 54; const u8 gfac = 182; const u8 bfac = 18;
  
  result.width = img->width; result.height = img->height;
  result.data = malloc(result.width*result.height * sizeof(u8));
  
  olPixel* og_data = img->data;
  for (int y = 0; y < result.height; ++y) {
    u32 row_index = y * result.width;
    for (int x = 0; x < result.width; ++x) {
      olPixel p = og_data[row_index+x];
      result.data[row_index + x] = p.r * rfac
                                 + p.g * gfac
                                 + p.b * bfac;
    }
  }
  }
  return result;
}

bool ol_is_whitespace(char c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\v';
} 

bool ol_is_numeric(char c) {
  return c >= '0' && c <= '9';
}

i64 ol_parse_int(char* c, i64 len, u32 base) {
  i64 result = 0;
  char* cur = c;
  for (int i = 0; i < len; i++) {
    result *= base;
    result += *cur - '0';
    cur++;
  }

  return result;
}

olImage olImage_load_ppm(olByteBuffer buf, olError* out_error) {
  u8* cur = buf.data;
  olImage result = (olImage) {.data = null, .width = 0, .height = 0};
  
  // P6
  u16 magic_num = read(u16, &cur);
  if (magic_num != 13904) {
    *out_error = olErr(olINVALID_MAGIC, 0);
    return result;
  }

  // skip whitespace
  char c;
  do {
    c = read(char, &cur);
  } while (ol_is_whitespace(c));

  // parse width
  char* num_start = (char*)cur;
  do {
    c = read(char, &cur);
  } while (ol_is_numeric(c));
  u32 len = (u64)cur - (u64)num_start;
  if (len == 0) {
    *out_error = olErr(olINVALIDFMT, 0);
    free(buf.data);
    return result;
  }
  u32 width = ol_parse_int(num_start, len, 10);
   
  // skip whitespace
  do {
    c = read(char, &cur);
  } while (ol_is_whitespace(c));

  // parse height
  num_start = (char*)cur;
  do {
    c = read(char, &cur);
  } while (ol_is_numeric(c));
  len = (u64)cur - (u64)num_start;
  if (len == 0) {
    *out_error = olErr(olINVALIDFMT, 0);
    free(buf.data);
    return result;
  }
  u32 height = ol_parse_int(num_start, len, 10);
   
  // skip whitespace
  do {
    c = read(char, &cur);
  } while (ol_is_whitespace(c));

  // get max val
  u16 max_val = read(u16, &cur);
  if (max_val == 0) {
    *out_error = olErr(olINVALIDFMT, 0);
    free(buf.data);
    return result;
  }

  // skip single whitespace
  read(char, &cur);

  result.data = malloc(width * height * sizeof(olPixel));
  u64 i = 0;
  // 1 byte or 2 byte per color component
  if (max_val < 256) {
    while (*cur != 0) {
      u8 r = read(u8, &cur);
      result.data[i++].r = (r / max_val) * 255;
      u8 g = read(u8, &cur);
      if (g == 0) {
        free(result.data);
        result.data = null;
        *out_error = olErr(olUNEXPECTEDEOF, 0);
        free(buf.data);
        return result;
      }
      result.data[i++].g = (g / max_val) * 255;
      u8 b = read(u8, &cur);
      if (g == 0) {
        free(result.data);
        result.data = null;
        *out_error = olErr(olUNEXPECTEDEOF, 0);
        free(buf.data);
        return result;
      }
      result.data[i++].b = (b / max_val) * 255;
    }
  } else {
    while (*cur != 0) {
      u16 r = read(u16, &cur);
      result.data[i++].r = (r / max_val) * 255;
      u16 g = read(u16, &cur);
      if (g == 0) {
        free(result.data);
        result.data = null;
        *out_error = olErr(olUNEXPECTEDEOF, 0);
        free(buf.data);
        return result;
      }
      result.data[i++].g = (g / max_val) * 255;
      u16 b = read(u16, &cur);
      if (g == 0) {
        free(result.data);
        result.data = null;
        *out_error = olErr(olUNEXPECTEDEOF, 0);
        free(buf.data);
        return result;
      }
      result.data[i++].b = (b / max_val) * 255;
    }
  }
  free(buf.data);
  return result;
}

// SECTION FILES & BYTEBUFFER
void* _readbytes(u8** ptr, u32 size) {
  void* result = *ptr;
  *ptr = advance_ptr(*ptr, size);
  return result;
}

olByteBuffer olFile_loadb(olStr* path, olError* out_error) {
#ifdef _WIN32
  HANDLE hfile = CreateFile(path->data,
                            GENERIC_READ,
                            FILE_SHARE_READ,
                            null,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            null);
  if (hfile == INVALID_HANDLE_VALUE) {
    out_error->kind = olFILENOTFOUND;
    out_error->data = 0;
    return (olByteBuffer){.data=null,.len=0};
  }

  LARGE_INTEGER file_size_out;
  if (!GetFileSizeEx(hfile, &file_size_out)) {
    CloseHandle(hfile);
    out_error->kind = olWINERROR;
    out_error->data = GetLastError();
    return (olByteBuffer){.data=null,.len=0};
  }
  u64 file_size = file_size_out.QuadPart;

  olByteBuffer result;
  result.data = malloc((file_size+1) * sizeof(u8));
  
  if (!ReadFile(hfile, result.data, file_size, (unsigned long*)&result.len, null)) {
    CloseHandle(hfile);
    out_error->kind = olWINERROR;
    out_error->data = GetLastError();
    free(result.data);
    result.data = null;
    return result;
  }
  
  return result;
#endif
}
