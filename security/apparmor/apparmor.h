/*
 *	Copyright (C) 1998-2007 Novell/SUSE
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2 of the
 *	License.
 *
 *	AppArmor internal prototypes
 */

#ifndef __APPARMOR_H
#define __APPARMOR_H

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/binfmts.h>
#include <linux/rcupdate.h>
#include <linux/socket.h>
#include <net/sock.h>

/*
 * We use MAY_READ, MAY_WRITE, MAY_EXEC, MAY_APPEND and the following flags
 * for profile permissions
 */
#define AA_MAY_LINK			0x0010
#define AA_MAY_LOCK			0x0020
#define AA_EXEC_MMAP			0x0040
#define AA_EXEC_UNSAFE			0x0080
#define AA_EXEC_MOD_0			0x0100
#define AA_EXEC_MOD_1			0x0200
#define AA_BASE_PERMS			(MAY_READ | MAY_WRITE | MAY_EXEC | \
					 MAY_APPEND | AA_MAY_LINK | \
					 AA_MAY_LOCK | AA_EXEC_MMAP | \
					 AA_EXEC_UNSAFE | AA_EXEC_MOD_0 | \
					 AA_EXEC_MOD_1)
#define AA_LINK_SUBSET_TEST		0x0020

#define AA_EXEC_UNCONFINED		0
#define AA_EXEC_INHERIT			AA_EXEC_MOD_0
#define AA_EXEC_PROFILE			AA_EXEC_MOD_1
#define AA_EXEC_PIX			(AA_EXEC_MOD_0 | AA_EXEC_MOD_1)

#define AA_EXEC_MODIFIERS		(AA_EXEC_MOD_0 | AA_EXEC_MOD_1)

#define AA_USER_SHIFT			0
#define AA_OTHER_SHIFT			10

#define AA_USER_PERMS			(AA_BASE_PERMS << AA_USER_SHIFT)
#define AA_OTHER_PERMS			(AA_BASE_PERMS << AA_OTHER_SHIFT)

#define AA_FILE_PERMS			(AA_USER_PERMS | AA_OTHER_PERMS)

#define AA_LINK_BITS			((AA_MAY_LINK << AA_USER_SHIFT) | \
					 (AA_MAY_LINK << AA_OTHER_SHIFT))

#define AA_USER_EXEC			(MAY_EXEC << AA_USER_SHIFT)
#define AA_OTHER_EXEC			(MAY_EXEC << AA_OTHER_SHIFT)

#define AA_USER_EXEC_MODS		(AA_EXEC_MODIFIERS << AA_USER_SHIFT)
#define AA_OTHER_EXEC_MODS		(AA_EXEC_MODIFIERS << AA_OTHER_SHIFT)

#define AA_USER_EXEC_UNSAFE		(AA_EXEC_UNSAFE << AA_USER_SHIFT)
#define AA_OTHER_EXEC_UNSAFE		(AA_EXEC_UNSAFE << AA_OTHER_SHIFT)

#define AA_EXEC_BITS			(AA_USER_EXEC | AA_OTHER_EXEC)

#define AA_ALL_EXEC_MODS		(AA_USER_EXEC_MODS | \
					 AA_OTHER_EXEC_MODS)

/* shared permissions that are not duplicated in user:group:other */
#define AA_CHANGE_PROFILE		0x40000000

#define AA_SHARED_PERMS			(AA_CHANGE_PROFILE)

#define AA_VALID_PERM_MASK		(AA_FILE_PERMS | AA_SHARED_PERMS)

#define AA_SECURE_EXEC_NEEDED		1

/* Control parameters (0 or 1), settable thru module/boot flags or
 * via /sys/kernel/security/apparmor/control */
extern int apparmor_complain;
extern int apparmor_debug;
extern int apparmor_audit;
extern int apparmor_logsyscall;
extern unsigned int apparmor_path_max;

#define PROFILE_COMPLAIN(_profile) \
	(apparmor_complain == 1 || ((_profile) && (_profile)->flags.complain))

#define APPARMOR_COMPLAIN(_cxt) \
	(apparmor_complain == 1 || \
	 ((_cxt) && (_cxt)->profile && (_cxt)->profile->flags.complain))

#define PROFILE_AUDIT(_profile) \
	(apparmor_audit == 1 || ((_profile) && (_profile)->flags.audit))

#define APPARMOR_AUDIT(_cxt) \
	(apparmor_audit == 1 || \
	 ((_cxt) && (_cxt)->profile && (_cxt)->profile->flags.audit))

/*
 * DEBUG remains global (no per profile flag) since it is mostly used in sysctl
 * which is not related to profile accesses.
 */

#define AA_DEBUG(fmt, args...)						\
	do {								\
		if (apparmor_debug)					\
			printk(KERN_DEBUG "AppArmor: " fmt, ##args);	\
	} while (0)

#define AA_ERROR(fmt, args...)	printk(KERN_ERR "AppArmor: " fmt, ##args)

struct aa_profile;

/* struct aa_namespace - namespace for a set of profiles
 * @name: the name of the namespace
 * @list: list the namespace is on
 * @profiles: list of profile in the namespace
 * @profile_count: the number of profiles in the namespace
 * @null_complain_profile: special profile used for learning in this namespace
 * @count: reference count on the namespace
 * @lock: lock for adding/removing profile to the namespace
 */
struct aa_namespace {
	char *name;
	struct list_head list;
	struct list_head profiles;
	int profile_count;
	struct aa_profile *null_complain_profile;

	struct kref count;
	rwlock_t lock;
};

/* struct aa_profile - basic confinement data
 * @name: the profiles name
 * @list: list this profile is on
 * @ns: namespace the profile is in
 * @file_rules: dfa containing the profiles file rules
 * @flags: flags controlling profile behavior
 * @isstale: flag indicating if profile is stale
 * @capabilities: capabilities granted by the process
 * @count: reference count of the profile
 *
 * The AppArmor profile contains the basic confinement data.  Each profile
 * has a name, and all nonstale profile are in a profile namespace.
 *
 * The task_contexts list and the isstale flag are protected by the
 * profile lock.
 *
 * If a task context is moved between two profiles, we first need to grab
 * both profile locks. lock_both_profiles() does that in a deadlock-safe
 * way.
 */
struct aa_profile {
	char *name;
	struct list_head list;
	struct aa_namespace *ns;

	struct aa_dfa *file_rules;
	struct {
		int complain;
		int audit;
	} flags;
	int isstale;

	kernel_cap_t capabilities;
	struct kref count;
	struct list_head task_contexts;
	spinlock_t lock;
	unsigned long int_flags;
	u16 network_families[AF_MAX];
};

extern struct list_head profile_ns_list;
extern rwlock_t profile_ns_list_lock;
extern struct mutex aa_interface_lock;

/**
 * struct aa_task_context - primary label for confined tasks
 * @profile: the current profile
 * @previous_profile: profile the task may return to
 * @cookie: magic value the task must know for returning to @previous_profile
 * @list: list this aa_task_context is on
 * @task: task that the aa_task_context confines
 * @rcu: rcu head used when freeing the aa_task_context
 * @caps_logged: caps that have previously generated log entries
 *
 * Contains the task's current profile (which could change due to
 * change_hat).  Plus the hat_magic needed during change_hat.
 */
struct aa_task_context {
	struct aa_profile *profile;
	struct aa_profile *previous_profile;
	u64 cookie;
	struct list_head list;
	struct task_struct *task;
	struct rcu_head rcu;
	kernel_cap_t caps_logged;
};

extern struct aa_namespace *default_namespace;

/* aa_audit - AppArmor auditing structure
 * Structure is populated by access control code and passed to aa_audit which
 * provides for a single point of logging.
 */

struct aa_audit {
	const char *operation;
	gfp_t gfp_mask;
	const char *info;
	const char *name;
	const char *name2;
	const char *name3;
	int request_mask, denied_mask;
	struct iattr *iattr;
	pid_t task, parent;
	int family, type, protocol;
	int error_code;
};

/* Flags for the permission check functions */
#define AA_CHECK_FD	1  /* coming from a file descriptor */
#define AA_CHECK_DIR	2  /* file type is directory */

/* lock subtypes so lockdep does not raise false dependencies */
enum aa_lock_class {
	aa_lock_normal,
	aa_lock_nested,
	aa_lock_task_release
};

/* main.c */
extern int alloc_default_namespace(void);
extern void free_default_namespace(void);
extern int aa_audit_message(struct aa_profile *profile, struct aa_audit *sa,
			    int type);
void aa_audit_hint(struct aa_profile *profile, struct aa_audit *sa);
void aa_audit_status(struct aa_profile *profile, struct aa_audit *sa);
int aa_audit_reject(struct aa_profile *profile, struct aa_audit *sa);
extern int aa_audit_syscallreject(struct aa_profile *profile, gfp_t gfp,
				  const char *);
extern int aa_audit(struct aa_profile *profile, struct aa_audit *);

extern int aa_attr(struct aa_profile *profile, struct dentry *dentry,
		   struct vfsmount *mnt, struct iattr *iattr);
extern int aa_perm_xattr(struct aa_profile *profile, const char *operation,
			 struct dentry *dentry, struct vfsmount *mnt,
			 int mask, int check);
extern int aa_capability(struct aa_task_context *cxt, int cap);
extern int aa_perm(struct aa_profile *profile, const char *operation,
		   struct dentry *dentry, struct vfsmount *mnt, int mask,
		   int check);
extern int aa_perm_dir(struct aa_profile *profile, const char *operation,
		       struct dentry *dentry, struct vfsmount *mnt,
		       int mask);
extern int aa_perm_path(struct aa_profile *, const char *operation,
			const char *name, int mask, uid_t uid);
extern int aa_link(struct aa_profile *profile,
		   struct dentry *link, struct vfsmount *link_mnt,
		   struct dentry *target, struct vfsmount *target_mnt);
extern int aa_clone(struct task_struct *task);
extern int aa_register(struct linux_binprm *bprm);
extern void aa_release(struct task_struct *task);
extern int aa_change_hat(const char *id, u64 hat_magic);
extern int aa_change_profile(const char *ns_name, const char *name);
extern struct aa_profile *__aa_replace_profile(struct task_struct *task,
					       struct aa_profile *profile);
extern struct aa_task_context *lock_task_and_profiles(struct task_struct *task,
						      struct aa_profile *profile);
extern void unlock_task_and_profiles(struct task_struct *task,
				     struct aa_task_context *cxt,
				     struct aa_profile *profile);
extern void aa_change_task_context(struct task_struct *task,
				   struct aa_task_context *new_cxt,
				   struct aa_profile *profile, u64 cookie,
				   struct aa_profile *previous_profile);
extern int aa_may_ptrace(struct aa_task_context *cxt,
			 struct aa_profile *tracee);
extern int aa_net_perm(struct aa_profile *profile, char *operation,
			int family, int type, int protocol);
extern int aa_revalidate_sk(struct sock *sk, char *operation);

/* lsm.c */
extern int apparmor_initialized;
extern void info_message(const char *str);
extern void apparmor_disable(void);

/* list.c */
extern struct aa_namespace *__aa_find_namespace(const char *name,
						struct list_head *list);
extern struct aa_profile *__aa_find_profile(const char *name,
					    struct list_head *list);
extern void aa_profile_ns_list_release(void);

/* module_interface.c */
extern ssize_t aa_add_profile(void *, size_t);
extern ssize_t aa_replace_profile(void *, size_t);
extern ssize_t aa_remove_profile(char *, size_t);
extern struct aa_namespace *alloc_aa_namespace(char *name);
extern void free_aa_namespace(struct aa_namespace *ns);
extern void free_aa_namespace_kref(struct kref *kref);
extern struct aa_profile *alloc_aa_profile(void);
extern void free_aa_profile(struct aa_profile *profile);
extern void free_aa_profile_kref(struct kref *kref);
extern void aa_unconfine_tasks(struct aa_profile *profile);

/* procattr.c */
extern int aa_getprocattr(struct aa_profile *profile, char **string,
			  unsigned *len);
extern int aa_setprocattr_changehat(char *args);
extern int aa_setprocattr_changeprofile(char *args);
extern int aa_setprocattr_setprofile(struct task_struct *task, char *args);

/* apparmorfs.c */
extern int create_apparmorfs(void);
extern void destroy_apparmorfs(void);

/* match.c */
extern struct aa_dfa *aa_match_alloc(void);
extern void aa_match_free(struct aa_dfa *dfa);
extern int unpack_dfa(struct aa_dfa *dfa, void *blob, size_t size);
extern int verify_dfa(struct aa_dfa *dfa);
extern unsigned int aa_dfa_match(struct aa_dfa *dfa, const char *str);
extern unsigned int aa_dfa_next_state(struct aa_dfa *dfa, unsigned int start,
				      const char *str);
extern unsigned int aa_match_state(struct aa_dfa *dfa, unsigned int start,
				   const char *str, unsigned int *final);
extern unsigned int aa_dfa_null_transition(struct aa_dfa *dfa,
					   unsigned int start);

#endif  /* __APPARMOR_H */
