#ifndef LOG_H
#define LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOG_VERSION "0.1.0"

typedef struct
{
  va_list    ap;
  const char *fmt;
  const char *file;
  struct tm *time;
  void      *udata;
  int        line;
  int        level;
} log_Event;

typedef void (*log_LogFn)(log_Event *ev);
typedef void (*log_LockFn)(bool lock, void *udata);

enum
{
  LOG_TRACE,
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR,
  LOG_FATAL
};

#define LOG_USE_COLOR 1

#define SHORT_FILE_NAME 1

/* 获取文件名（不含路径） */
#define __SHORT_FILE__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

#ifdef SHORT_FILE_NAME
  #define FILE_NAME __SHORT_FILE__
#else
  #define _FILE_NAME_ __FILE__
#endif

#define log_trace(...) log_log(LOG_TRACE, FILE_NAME, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, FILE_NAME, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(LOG_INFO, FILE_NAME, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(LOG_WARN, FILE_NAME, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, FILE_NAME, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, FILE_NAME, __LINE__, __VA_ARGS__)

const char *log_level_string(int level);
void        log_set_lock(log_LockFn fn, void *udata);
void        log_set_level(int level);
void        log_set_quiet(bool enable);
int         log_add_callback(log_LogFn fn, void *udata, int level);
int         log_add_fp(FILE *fp, int level);

void log_log(int level, const char *file, int line, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
