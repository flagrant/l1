
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/fs.h>

struct enum_entry {
	const char *name;
	long value;
};

void
print_enum(const char *name,struct enum_entry entries[]) {
	struct enum_entry *cursor;

	printf("enum %s {\n", name);
        cursor=entries;
        for(cursor=entries;*(void **)cursor && cursor->name;cursor++) {
		printf("\t%s\t= 0x%x,\n",cursor->name,cursor->value);
        }
	printf("};\n");
}

int
main(int argc,char **argv) {

	struct enum_entry oflags[]={
        	{ "O_RDONLY",	O_RDONLY },
		{ "O_WRONLY",	O_WRONLY },
		{ "O_RDWR", 	O_RDWR },	
		{ "O_CREAT", 	O_CREAT },	
		{ "O_TRUNC", 	O_TRUNC },	
		{ "O_APPEND", 	O_APPEND },	
		{ "O_NONBLOCK", O_NONBLOCK },	
		{ NULL },
        };

	struct enum_entry mmap_consts[]={
		{ "PROT_READ",		PROT_READ },
		{ "PROT_WRITE",		PROT_WRITE },
		{ "MAP_SHARED",		MAP_SHARED },
		{ "MAP_PRIVATE",	MAP_PRIVATE },
		{ "MAP_LOCKED",		MAP_LOCKED },
		{ "MAP_NORESERVE",	MAP_NORESERVE },
		{ NULL },
	};

	struct enum_entry address_families[]={
		{ "AF_UNSPEC",		AF_UNSPEC },
		{ "AF_UNIX",		AF_UNIX },
		{ "AF_LOCAL",		AF_LOCAL },
		{ "AF_INET",		AF_INET },
		{ "AF_INET6",		AF_INET6 },
		{ NULL },

	};

	struct enum_entry socket_type[]={
		{ "SOCK_STREAM",	SOCK_STREAM },
		{ "SOCK_DGRAM",		SOCK_DGRAM },
		{ "SOCK_RAW",		SOCK_RAW },
		{ NULL },
	};

	struct enum_entry ioctl_nr[]={
		{ "BLKGETSIZE",		BLKGETSIZE },
		{ NULL },
	};

	struct enum_entry syscall_nr[]={
		{ "read",	SYS_read },
		{ "write",	SYS_write },
		{ "open",	SYS_open },
		{ "close",	SYS_close },
		{ "stat",	SYS_stat },
		{ "fstat",	SYS_fstat },
		{ "lstat",	SYS_lstat },
		{ "poll",	SYS_poll },
		{ "lseek",	SYS_lseek },
		{ "mmap",	SYS_mmap },
		{ "mprotect",	SYS_mprotect },
		{ "munmap",	SYS_munmap },
		{ "brk",	SYS_brk },
		{ "rt_sigaction",	SYS_rt_sigaction },
		{ "rt_sigprocmask",	SYS_rt_sigprocmask },
		{ "rt_sigreturn",	SYS_rt_sigreturn },
		{ "ioctl",		SYS_ioctl },
		{ "pread64",		SYS_pread64 },
		{ "pwrite64",		SYS_pwrite64 },
		{ "readv",		SYS_readv },
		{ "writev",		SYS_writev },
		{ "access",		SYS_access },
		{ "pipe",		SYS_pipe },
		{ "select",		SYS_select },
		{ "sched_yield",	SYS_sched_yield },
		{ "mremap",		SYS_mremap },
		{ "msync",		SYS_msync },
		{ "mincore",		SYS_mincore },
		{ "madvise",		SYS_madvise },
#ifdef SYS_shmget // linux i386 misses these
		{ "shmget",		SYS_shmget },
		{ "shmat",		SYS_shmat },
		{ "shmctl",		SYS_shmctl },
#endif
		{ "dup",		SYS_dup },
		{ "dup2",		SYS_dup2 },
		{ "pause",		SYS_pause },
		{ "nanosleep",		SYS_nanosleep },
		{ "getitimer",		SYS_getitimer },
		{ "alarm",		SYS_alarm },
		{ "setitimer",		SYS_setitimer },
		{ "getpid",		SYS_getpid },
		{ "sendfile",		SYS_sendfile },
#ifdef SYS_socket // linux i386 uses socketcall
		{ "socket",		SYS_socket },
		{ "connect",		SYS_connect },
		{ "accept",		SYS_accept },
		{ "sendto",		SYS_sendto },
		{ "recvfrom",		SYS_recvfrom },
		{ "sendmsg",		SYS_sendmsg },
		{ "recvmsg",		SYS_recvmsg },
		{ "shutdown",		SYS_shutdown },
		{ "bind",		SYS_bind },
		{ "listen",		SYS_listen },
		{ "getsockname",	SYS_getsockname },
		{ "getpeername",	SYS_getpeername },
		{ "socketpair",		SYS_socketpair },
		{ "setsockopt",		SYS_setsockopt },
		{ "getsockopt",		SYS_getsockopt },
#endif
		{ "clone",		SYS_clone },
		{ "fork",		SYS_fork },
		{ "vfork",		SYS_vfork },
		{ "execve",		SYS_execve },
		{ "exit",		SYS_exit },
		{ "wait4",		SYS_wait4 },
		{ "kill",		SYS_kill },
		{ "uname",		SYS_uname },
#ifdef SYS_shmget // linux i386 misses these
		{ "semget",		SYS_semget },
		{ "semop",		SYS_semop },
		{ "semctl",		SYS_semctl },
		{ "shmdt",		SYS_shmdt },
		{ "msgget",		SYS_msgget },
		{ "msgsnd",		SYS_msgsnd },
		{ "msgrcv",		SYS_msgrcv },
		{ "msgctl",		SYS_msgctl },
#endif
		{ "fcntl",		SYS_fcntl },
		{ "flock",		SYS_flock },
		{ "fsync",		SYS_fsync },
		{ "fdatasync",		SYS_fdatasync },
		{ "truncate",		SYS_truncate },
		{ "ftruncate",		SYS_ftruncate },
		{ "getdents",		SYS_getdents },
		{ "getcwd",		SYS_getcwd },
		{ "chdir",		SYS_chdir },
		{ "fchdir",		SYS_fchdir },
		{ "rename",		SYS_rename },
		{ "mkdir",		SYS_mkdir },
		{ "rmdir",		SYS_rmdir },
		{ "creat",		SYS_creat },
		{ "link",		SYS_link },
		{ "unlink",		SYS_unlink },
		{ "symlink",		SYS_symlink },
		{ "readlink",		SYS_readlink },
		{ "chmod",		SYS_chmod },
		{ "fchmod",		SYS_fchmod },
		{ "chown",		SYS_chown },
		{ "fchown",		SYS_fchown },
		{ "lchown",		SYS_lchown },
		{ "umask",		SYS_umask },
		{ "gettimeofday",	SYS_gettimeofday },
		{ "getrlimit",		SYS_getrlimit },
		{ "getrusage",		SYS_getrusage },
		{ "sysinfo",		SYS_sysinfo },
		{ "times",		SYS_times },
		{ "ptrace",		SYS_ptrace },
		{ "getuid",		SYS_getuid },
		{ "syslog",		SYS_syslog },
		{ "getgid",		SYS_getgid },
		{ "setuid",		SYS_setuid },
		{ "setgid",		SYS_setgid },
		{ "geteuid",		SYS_geteuid },
		{ "getegid",		SYS_getegid },
		{ "setpgid",		SYS_setpgid },
		{ "getppid",		SYS_getppid },
		{ "getpgrp",		SYS_getpgrp },
		{ "setsid",		SYS_setsid },
		{ "setreuid",		SYS_setreuid },
		{ "setregid",		SYS_setregid },
		{ "getgroups",		SYS_getgroups },
		{ "setgroups",		SYS_setgroups },
		{ "setresuid",		SYS_setresuid },
		{ "getresuid",		SYS_getresuid },
		{ "setresgid",		SYS_setresgid },
		{ "getresgid",		SYS_getresgid },
		{ "getpgid",		SYS_getpgid },
		{ "setfsuid",		SYS_setfsuid },
		{ "setfsgid",		SYS_setfsgid },
		{ "getsid",		SYS_getsid },
		{ "capget",		SYS_capget },
		{ "capset",		SYS_capset },
		{ "rt_sigpending",	SYS_rt_sigpending },
		{ "rt_sigtimedwait",	SYS_rt_sigtimedwait },
		//{ "rt_sigqueueinfo",	SYS_sigqueueinfo },
		//{ "rt_sigsuspend",	SYS_sigsuspend },
		//{ "signaltstack",	SYS_signalstack },
		{ "utime",		SYS_utime },
		{ "mknod",		SYS_mknod },
		{ "uselib",		SYS_uselib },
		{ "personality",	SYS_personality },
		{ "ustat",		SYS_ustat },
		{ "statfs",		SYS_statfs },
		{ "fstatfs",		SYS_fstatfs },
		{ "sysfs",		SYS_sysfs },
		{ "getpriority",	SYS_getpriority },
		{ "setpriority",		SYS_setpriority },
		{ "sched_setparam",		SYS_sched_setparam },
		{ "sched_getparam",		SYS_sched_getparam },
		{ "sched_setscheduler",		SYS_sched_setscheduler },
		{ "sched_getscheduler",		SYS_sched_getscheduler },
		{ "sched_get_priority_max",	SYS_sched_get_priority_max },
		{ "sched_get_priority_min",	SYS_sched_get_priority_min },
		{ "sched_rr_get_interval",	SYS_sched_rr_get_interval },
		{ "mlock",		SYS_mlock },
		{ "munlock",		SYS_munlock },
		{ "mlockall",		SYS_mlockall },
		{ "munlockall",		SYS_munlockall },
		{ "vhangup",		SYS_vhangup },
		{ "modify_ldt",		SYS_modify_ldt },
		{ "pivot_root",		SYS_pivot_root },
		//{ "_sysctl",		SYS_
		{ "prctl",		SYS_prctl },
#ifdef SYS_arch_prctl // linux i386 misses this
		{ "arch_prctl",		SYS_arch_prctl },
#endif
		{ "adjtimex",		SYS_adjtimex },
		{ "setrlimit",		SYS_setrlimit },
		{ "chroot",		SYS_chroot },
		{ "sync",		SYS_sync },
		{ "acct",		SYS_acct },
		{ "settimeofday",	SYS_settimeofday },
		{ "mount",		SYS_mount },
		{ "umount2",		SYS_umount2 },
		{ "swapon",		SYS_swapon },
		{ "swapoff",		SYS_swapoff },
		{ "reboot",		SYS_reboot },
		{ "sethostname",	SYS_sethostname },
		{ "setdomainname",	SYS_setdomainname },
		{ "iopl",		SYS_iopl },
		{ "ioperm",		SYS_ioperm },
		{ "create_module",	SYS_create_module },
		{ "init_module",	SYS_init_module },
		{ "delete_module",	SYS_delete_module },
		{ "get_kernel_syms",	SYS_get_kernel_syms },
		{ "query_module",	SYS_query_module },
		{ "quotactl",		SYS_quotactl },
		{ "nfsservctl",		SYS_nfsservctl },
		{ "getpmsg",		SYS_getpmsg },
		{ "putpmsg",		SYS_putpmsg },
		{ "afs_syscall",	SYS_afs_syscall },
#ifdef SYS_tuxcall // linux i386 misses this
		{ "tuxcall",		SYS_tuxcall },
#endif
#ifdef SYS_security // linux i386 misses this
		{ "security",		SYS_security },
#endif
		{ "gettid",		SYS_gettid },
		{ "readahead",		SYS_readahead },
		{ "setxattr",		SYS_setxattr },
		{ "lsetxattr",		SYS_lsetxattr },
		{ "fsetxattr",		SYS_fsetxattr },
		{ "getxattr",		SYS_getxattr },
		{ "lgetxattr",		SYS_lgetxattr },
		{ "fgetxattr",		SYS_fgetxattr },
		{ "listxattr",		SYS_listxattr },
		{ "llistxattr",		SYS_llistxattr },
		{ "flistxattr",		SYS_flistxattr },
		{ "removexattr",	SYS_removexattr },
		{ "lremovexattr",	SYS_lremovexattr },
		{ "fremovexattr",	SYS_fremovexattr },
		{ "tkill",		SYS_tkill },
		{ "time",		SYS_time },
		{ "futex",		SYS_futex },
		{ "sched_setaffinity",	SYS_sched_setaffinity },
		{ "sched_getaffinity",	SYS_sched_getaffinity },
		{ "set_thread_area",	SYS_set_thread_area },
		{ "io_setup",		SYS_io_setup },
		{ "io_destroy",		SYS_io_destroy },
		{ "io_getevents",	SYS_io_getevents },
		{ "io_submit",		SYS_io_submit },
		{ "io_cancel",		SYS_io_cancel },
		{ "get_thread_area",	SYS_get_thread_area },
		{ "lookup_dcookie",	SYS_lookup_dcookie },
		{ "epoll_create",	SYS_epoll_create },
#ifdef SYS_epoll_ctl_old // linux i386 misses this
		{ "epoll_ctl_old",	SYS_epoll_ctl_old },
		{ "epoll_wait_old",	SYS_epoll_wait_old },
#endif
		{ "remap_file_pages",	SYS_remap_file_pages },
		{ "getdents64",		SYS_getdents64 },
		{ "set_tid_address",	SYS_set_tid_address },
		{ "restart_syscall",	SYS_restart_syscall },
#ifdef SYS_semtimedop
		{ "semtimedop",		SYS_semtimedop },
#endif
		{ "fadvise64",		SYS_fadvise64 },
		{ "timer_create",	SYS_timer_create },
		{ "timer_settime",	SYS_timer_settime },
		{ "timer_gettime",	SYS_timer_gettime },
		{ "timer_getoverrun",	SYS_timer_getoverrun },
		{ "timer_delete",	SYS_timer_delete },
		{ "clock_settime",	SYS_clock_settime },
		{ "clock_gettime",	SYS_clock_gettime },
		{ "clock_getres",	SYS_clock_getres },
		{ "clock_nanosleep",	SYS_clock_nanosleep },
		{ "exit_group",		SYS_exit_group },
		{ "epoll_wait",		SYS_epoll_wait },
		{ "epoll_ctl",		SYS_epoll_ctl },
		{ "tgkill",		SYS_tgkill },
		{ "utimes",		SYS_utimes },
		{ "vserver",		SYS_vserver },
		{ "mbind",		SYS_mbind },
		{ "set_mempolicy",	SYS_set_mempolicy },
		{ "get_mempolicy",	SYS_get_mempolicy },
		{ "mq_open",		SYS_mq_open },
		{ "mq_unlink",		SYS_mq_unlink },
		{ "mq_timedsend",	SYS_mq_timedsend },
		{ "mq_timedreceive",	SYS_mq_timedreceive },
		{ "mq_notify",		SYS_mq_notify },
		{ "mq_getsetattr",	SYS_mq_getsetattr },
		{ "kexec_load",		SYS_kexec_load },
		{ "waitid",		SYS_waitid },
		{ "add_key",		SYS_add_key },
		{ "request_key",	SYS_request_key },
		{ "keyctl",		SYS_keyctl },
		{ "ioprio_set",		SYS_ioprio_set },
		{ "ioprio_get",		SYS_ioprio_get },
		{ "inotify_init",	SYS_inotify_init },
		{ "inotify_add_watch",	SYS_inotify_add_watch },
		{ "inotify_rm_watch",	SYS_inotify_rm_watch },
		{ "migrate_pages",	SYS_migrate_pages },
		{ "openat",		SYS_openat },
		{ "mkdirat",		SYS_mkdirat },
		{ "mknodat",		SYS_mknodat },
		{ "fchownat",		SYS_fchownat },
		{ "futimesat",		SYS_futimesat },
#ifdef SYS_semtimedop // linux i386 misses this
		{ "newfstatat",		SYS_newfstatat },
#endif
		{ "unlinkat",		SYS_unlinkat },
		{ "renameat",		SYS_renameat },
		{ "linkat",		SYS_linkat },
		{ "symlinkat",		SYS_symlinkat },
		{ "readlinkat",		SYS_readlinkat },
		{ "fchmodat",		SYS_fchmodat },
		{ "faccessat",		SYS_faccessat },
		{ "pselect6",		SYS_pselect6 },
		{ "ppoll",		SYS_ppoll },
		{ "unshare",		SYS_unshare },
		{ "set_robust_list",	SYS_set_robust_list },
		{ "get_robust_list",	SYS_get_robust_list },
		{ "splice",		SYS_splice },
		{ "tee",		SYS_tee },
		{ "sync_file_range",	SYS_sync_file_range },
		{ "vmsplice",		SYS_vmsplice },
		{ "move_pages",		SYS_move_pages },
		{ "utimensat",		SYS_utimensat },
		{ "epoll_pwait",	SYS_epoll_pwait },
		{ "signalfd",		SYS_signalfd },
		//{ "timerfd",		SYS_timerfd },
		{ "eventfd",		SYS_eventfd },
		{ "fallocate",		SYS_fallocate },
		{ NULL },
	};

	printf("/* generated automatically by mkunix */\n\n");
	print_enum("oflags", oflags);
	print_enum("mmap_consts",mmap_consts);
	print_enum("address_families",address_families);
	print_enum("socket_type",socket_type);
	print_enum("ioctl_nr",ioctl_nr);
	print_enum("syscall_nr",syscall_nr);

	return 0;

}
