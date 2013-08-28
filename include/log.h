/*
 * File:   log.h
 * Author: mike
 *
 * Created on August 26, 2013, 10:15 PM
 */

#ifndef LOG_H
#define	LOG_H

#ifdef	__cplusplus
extern "C" {
#endif

extern int loglevel;
#define ERROR(...) do {if (loglevel >= 1) zclock_log("E: " __VA_ARGS__);} while(0)
#define WARN(...) do {if (loglevel >= 2) zclock_log("W: "  __VA_ARGS__);} while(0)
#define INFO(...) do {if (loglevel >= 3) zclock_log("I: "  __VA_ARGS__);} while(0)
#define NOTE(...) do {if (loglevel >= 4) zclock_log("N: "  __VA_ARGS__);} while(0)
#define TRACE(...) do {if (loglevel >= 5) zclock_log("N: "  __VA_ARGS__);} while(0)

#ifdef	__cplusplus
}
#endif

#endif	/* LOG_H */

