/*
 * $Id$
 *
 * Helper functions related to fetching process information. Used by _psutil_osx
 * module methods.
 */

#include <Python.h>


typedef struct kinfo_proc kinfo_proc;

int get_proc_list(kinfo_proc **procList, size_t *procCount);
int get_kinfo_proc(pid_t pid, struct kinfo_proc *kp);
int get_argmax(void);
int pid_exists(long pid);
PyObject* get_arg_list(long pid);
