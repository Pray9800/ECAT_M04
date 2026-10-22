#include "logc.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif


#define MAX_CALLBACKS 32

typedef struct
{
  log_LogFn fn;
  void* udata;
  int level;
} Callback;

static struct
{
  void* udata;
  log_LockFn lock;
  int level;
  bool quiet;
  Callback callbacks[MAX_CALLBACKS];
} L;

static const char* level_strings[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

#ifdef LOG_USE_COLOR
static const char* level_colors[] = { "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m" };
#endif

static void stdout_callback(log_Event* ev)
{
  char buf[100] = { 0 };
#ifdef _WIN32
  SYSTEMTIME st;
  GetLocalTime(&st);
  sprintf(buf, "%d-%02d-%02d %02d:%02d:%02d.%03d [%lu]", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm* tm_info = localtime(&tv.tv_sec);
  char time_buffer[32] = { 0 };
  strftime(time_buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
  // sprintf(buf, "%s.%03d [%lu]", time_buffer, (long)tv.tv_usec / 1000);
  sprintf(buf, "%s.%03lu", time_buffer, tv.tv_usec / 1000);
#endif

#ifdef LOG_USE_COLOR
  fprintf((FILE*)ev->udata, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ", buf, level_colors[ev->level], level_strings[ev->level], ev->file, ev->line);
#else
  fprintf((FILE*)ev->udata, "%s %-5s %s:%d: ", buf, level_strings[ev->level], ev->file, ev->line);
#endif
  vfprintf((FILE*)ev->udata, ev->fmt, ev->ap);
  fprintf((FILE*)ev->udata, "\n");
  fflush((FILE*)ev->udata);
}

static void file_callback(log_Event* ev)
{
  char buf[100] = { 0 };
#ifdef _WIN32
  SYSTEMTIME st;
  GetLocalTime(&st);
  sprintf(buf, "%d-%02d-%02d %02d:%02d:%02d.%03d [%lu]", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm* tm_info = localtime(&tv.tv_sec);
  char time_buffer[32] = { 0 };
  strftime(time_buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
  // sprintf(buf, "%s.%03d [%lu]", time_buffer, (long)tv.tv_usec / 1000);
  sprintf(buf, "%s.%03lu", time_buffer, tv.tv_usec / 1000);
#endif

  fprintf((FILE*)ev->udata, "%s %-5s %s:%d: ", buf, level_strings[ev->level], ev->file, ev->line);
  vfprintf((FILE*)ev->udata, ev->fmt, ev->ap);
  fprintf((FILE*)ev->udata, "\n");
  fflush((FILE*)ev->udata);
}

static void lock(void)
{
  if (L.lock)
  {
    L.lock(true, L.udata);
  }
}

static void unlock(void)
{
  if (L.lock)
  {
    L.lock(false, L.udata);
  }
}

const char* log_level_string(int level) { return level_strings[level]; }

void log_set_lock(log_LockFn fn, void* udata)
{
  L.lock = fn;
  L.udata = udata;
}

void log_set_level(int level) { L.level = level; }

void log_set_quiet(bool enable) { L.quiet = enable; }

int log_add_callback(log_LogFn fn, void* udata, int level)
{
  for (int i = 0; i < MAX_CALLBACKS; i++)
  {
    if (!L.callbacks[i].fn)
    {
      L.callbacks[i].fn = fn;
      L.callbacks[i].udata = udata;
      L.callbacks[i].level = level;
      return 0;
    }
  }
  return -1;
}

int log_add_fp(FILE* fp, int level) { return log_add_callback(file_callback, fp, level); }

static void init_event(log_Event* ev, void* udata)
{
  if (!ev->time)
  {
    time_t t = time(NULL);
    ev->time = gmtime(&t);
  }
  ev->udata = udata;
}

void log_log(int level, const char* file, int line, const char* fmt, ...)
{
  log_Event ev;
  ev.fmt = fmt;
  ev.file = file;
  ev.line = line;
  ev.level = level;

  lock();

  if (!L.quiet && level >= L.level)
  {
    init_event(&ev, stderr);
    va_start(ev.ap, fmt);
    stdout_callback(&ev);
    va_end(ev.ap);
  }

  for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++)
  {
    Callback* cb = &L.callbacks[i];
    if (level >= cb->level)
    {
      init_event(&ev, cb->udata);
      va_start(ev.ap, fmt);
      cb->fn(&ev);
      va_end(ev.ap);
    }
  }

  unlock();
}
