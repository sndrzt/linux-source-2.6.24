/*
 * cpufreq driver for Enhanced SpeedStep, as found in Intel's Pentium
 * M (part of the Centrino chipset).
 *
 * Since the original Pentium M, most new Intel CPUs support Enhanced
 * SpeedStep.
 *
 * Despite the "SpeedStep" in the name, this is almost entirely unlike
 * traditional SpeedStep.
 *
 * Modelled on speedstep.c
 *
 * Copyright (C) 2003 Jeremy Fitzhardinge <jeremy@goop.org>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>	/* current */
#include <linux/delay.h>
#include <linux/compiler.h>

#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>

#define PFX		"speedstep-centrino: "
#define MAINTAINER	"cpufreq@lists.linux.org.uk"

#define dprintk(msg...) cpufreq_debug_printk(CPUFREQ_DEBUG_DRIVER, "speedstep-centrino", msg)

#define INTEL_MSR_RANGE	(0xffff)

struct cpu_id
{
	__u8	x86;            /* CPU family */
	__u8	x86_model;	/* model */
	__u8	x86_mask;	/* stepping */
};

enum {
	CPU_BANIAS,
	CPU_DOTHAN_A1,
	CPU_DOTHAN_A2,
	CPU_DOTHAN_B0,
	CPU_DOTHAN_C0,
	CPU_MP4HT_D0,
	CPU_MP4HT_E0,
};

static const struct cpu_id cpu_ids[] = {
	[CPU_BANIAS]	= { 6,  9, 5 },
	[CPU_DOTHAN_A1]	= { 6, 13, 1 },
	[CPU_DOTHAN_A2]	= { 6, 13, 2 },
	[CPU_DOTHAN_B0]	= { 6, 13, 6 },
	[CPU_DOTHAN_C0]	= { 6, 13, 8 },
	[CPU_MP4HT_D0]	= {15,  3, 4 },
	[CPU_MP4HT_E0]	= {15,  4, 1 },
};
#define N_IDS	ARRAY_SIZE(cpu_ids)

struct cpu_model
{
	const struct cpu_id *cpu_id;
	const char	*model_name;
	unsigned	max_freq; /* max clock in kHz */

	struct cpufreq_frequency_table *op_points; /* clock/voltage pairs */
};
static int centrino_verify_cpu_id(const struct cpuinfo_x86 *c, const struct cpu_id *x);

/* Operating points for current CPU */
static struct cpu_model *centrino_model[NR_CPUS];
static const struct cpu_id *centrino_cpu[NR_CPUS];

static struct cpufreq_driver centrino_driver;

#ifdef CONFIG_X86_SPEEDSTEP_CENTRINO_TABLE

/* Computes the correct form for IA32_PERF_CTL MSR for a particular
   frequency/voltage operating point; frequency in MHz, volts in mV.
   This is stored as "index" in the structure. */
#define OP(mhz, mv)							\
	{								\
		.frequency = (mhz) * 1000,				\
		.index = (((mhz)/100) << 8) | ((mv - 700) / 16)		\
	}

/*
 * These voltage tables were derived from the Intel Pentium M
 * datasheet, document 25261202.pdf, Table 5.  I have verified they
 * are consistent with my IBM ThinkPad X31, which has a 1.3GHz Pentium
 * M.
 */

/* Ultra Low Voltage Intel Pentium M processor 900MHz (Banias) */
static struct cpufreq_frequency_table banias_900[] =
{
	OP(600,  844),
	OP(800,  988),
	OP(900, 1004),
	{ .frequency = CPUFREQ_TABLE_END }
};

/* Ultra Low Voltage Intel Pentium M processor 1000MHz (Banias) */
static struct cpufreq_frequency_table banias_1000[] =
{
	OP(600,   844),
	OP(800,   972),
	OP(900,   988),
	OP(1000, 1004),
	{ .frequency = CPUFREQ_TABLE_END }
};

/* Low Voltage Intel Pentium M processor 1.10GHz (Banias) */
static struct cpufreq_frequency_table banias_1100[] =
{
	OP( 600,  956),
	OP( 800, 1020),
	OP( 900, 1100),
	OP(1000, 1164),
	OP(1100, 1180),
	{ .frequency = CPUFREQ_TABLE_END }
};


/* Low Voltage Intel Pentium M processor 1.20GHz (Banias) */
static struct cpufreq_frequency_table banias_1200[] =
{
	OP( 600,  956),
	OP( 800, 1004),
	OP( 900, 1020),
	OP(1000, 1100),
	OP(1100, 1164),
	OP(1200, 1180),
	{ .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 1.30GHz (Banias) */
static struct cpufreq_frequency_table banias_1300[] =
{
	OP( 600,  956),
	OP( 800, 1260),
	OP(1000, 1292),
	OP(1200, 1356),
	OP(1300, 1388),
	{ .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 1.40GHz (Banias) */
static struct cpufreq_frequency_table banias_1400[] =
{
	OP( 600,  956),
	OP( 800, 1180),
	OP(1000, 1308),
	OP(1200, 1436),
	OP(1400, 1484),
	{ .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 1.50GHz (Banias) */
static struct cpufreq_frequency_table banias_1500[] =
{
	OP( 600,  956),
	OP( 800, 1116),
	OP(1000, 1228),
	OP(1200, 1356),
	OP(1400, 1452),
	OP(1500, 1484),
	{ .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 1.60GHz (Banias) */
static struct cpufreq_frequency_table banias_1600[] =
{
	OP( 600,  956),
	OP( 800, 1036),
	OP(1000, 1164),
	OP(1200, 1276),
	OP(1400, 1420),
	OP(1600, 1484),
	{ .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 1.70GHz (Banias) */
static struct cpufreq_frequency_table banias_1700[] =
{
	OP( 600,  956),
	OP( 800, 1004),
	OP(1000, 1116),
	OP(1200, 1228),
	OP(1400, 1308),
	OP(1700, 1484),
	{ .frequency = CPUFREQ_TABLE_END }
};
#undef OP


#define OPEX(mhz, base, mva, mvb, mvc, mvd)                     \
{                                                               \
        .frequency = (mhz) * 1000,                              \
        .index = (((mhz)/(base)) << 8) | ((mva - 700) / 16)     \
}

/* Intel Pentium M processor 730 / 1.60 GHz (Sonoma) */
static struct cpufreq_frequency_table sonoma_1596[] =
{
        OPEX( 798, 133,  988,  988,  988,  988),
        OPEX(1064, 133, 1116, 1111, 1084, 1079),
        OPEX(1330, 133, 1244, 1233, 1180, 1169),
        OPEX(1596, 133, 1356, 1356, 1260, 1260),
        { .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 740 / 1.73 GHz (Sonoma) */
static struct cpufreq_frequency_table sonoma_1729[] =
{
        OPEX( 798, 133,  988,  988,  988,  988),
        OPEX(1064, 133, 1100, 1093, 1068, 1066),
        OPEX(1330, 133, 1212, 1198, 1148, 1143),
        OPEX(1729, 133, 1356, 1356, 1260, 1260),
        { .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 750 / 1.86 GHz (Sonoma) */
static struct cpufreq_frequency_table sonoma_1862[] =
{
        OPEX( 798, 133,  988,  988,  988,  988),
        OPEX(1064, 133, 1084, 1080, 1068, 1056),
        OPEX(1330, 133, 1180, 1172, 1132, 1124),
        OPEX(1596, 133, 1276, 1264, 1196, 1192),
        OPEX(1862, 133, 1356, 1356, 1260, 1260),
        { .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 760 / 2.00 GHz (Sonoma) */
static struct cpufreq_frequency_table sonoma_1995[] =
{
        OPEX( 798, 133, 988, 988, 988, 988),
        OPEX(1064, 133, 1084, 1070, 1052, 1048),
        OPEX(1330, 133, 1164, 1152, 1116, 1109),
        OPEX(1596, 133, 1244, 1233, 1180, 1169),
        OPEX(1995, 133, 1356, 1356, 1260, 1260),
        { .frequency = CPUFREQ_TABLE_END }
};
/* Intel Pentium M processor 770 / 2.13 GHz (Sonoma) */
static struct cpufreq_frequency_table sonoma_2128[] =
{
        OPEX( 798, 133, 988, 988, 988, 988),
        OPEX(1064, 133, 1068, 1065, 1052, 1042),
        OPEX(1330, 133, 1148, 1142, 1100, 1097),
        OPEX(1596, 133, 1228, 1218, 1164, 1151),
        OPEX(1862, 133, 1308, 1295, 1212, 1206),
        OPEX(2128, 133, 1372, 1372, 1260, 1260),
        { .frequency = CPUFREQ_TABLE_END }
};

/* Intel Pentium M processor 780 / 2.26 GHz (Sonoma) */
static struct cpufreq_frequency_table sonoma_2261[] =
{
        OPEX( 798, 133, 988, 988, 988, 988),
        OPEX(1064, 133, 1068, 1064, 1052, 1037),
        OPEX(1330, 133, 1148, 1139, 1100, 1087),
        OPEX(1596, 133, 1228, 1215, 1148, 1136),
        OPEX(1862, 133, 1292, 1291, 1196, 1186),
        OPEX(2261, 133, 1404, 1404, 1260, 1260),
        { .frequency = CPUFREQ_TABLE_END }
};

#undef OPEX

#define SONOMA(cpuid, max, base, name)  \
{       .cpu_id         = cpuid,        \
        .model_name     = "Intel(R) Pentium(R) M processor " name "GHz", \
        .max_freq       = (max)*1000,   \
        .op_points      = sonoma_##max, \
}


#define _BANIAS(cpuid, max, name)	\
{	.cpu_id		= cpuid,	\
	.model_name	= "Intel(R) Pentium(R) M processor " name "MHz", \
	.max_freq	= (max)*1000,	\
	.op_points	= banias_##max,	\
}
#define BANIAS(max)	_BANIAS(&cpu_ids[CPU_BANIAS], max, #max)

/* CPU models, their operating frequency range, and freq/voltage
   operating points */
static struct cpu_model models[] =
{
	_BANIAS(&cpu_ids[CPU_BANIAS], 900, " 900"),
	BANIAS(1000),
	BANIAS(1100),
	BANIAS(1200),
	BANIAS(1300),
	BANIAS(1400),
	BANIAS(1500),
	BANIAS(1600),
	BANIAS(1700),

        /* Builtin tables for Dothan C0 CPUs, a.k.a Sonoma */
        SONOMA(&cpu_ids[CPU_DOTHAN_C0], 1596, 133, "1.60"),
        SONOMA(&cpu_ids[CPU_DOTHAN_C0], 1729, 133, "1.73"),
        SONOMA(&cpu_ids[CPU_DOTHAN_C0], 1862, 133, "1.86"),
        SONOMA(&cpu_ids[CPU_DOTHAN_C0], 1995, 133, "2.00"),
        SONOMA(&cpu_ids[CPU_DOTHAN_C0], 2128, 133, "2.13"),
        SONOMA(&cpu_ids[CPU_DOTHAN_C0], 2261, 133, "2.26"),


	/* NULL model_name is a wildcard */
	{ &cpu_ids[CPU_DOTHAN_A1], NULL, 0, NULL },
	{ &cpu_ids[CPU_DOTHAN_A2], NULL, 0, NULL },
	{ &cpu_ids[CPU_DOTHAN_B0], NULL, 0, NULL },
	{ &cpu_ids[CPU_MP4HT_D0], NULL, 0, NULL },
	{ &cpu_ids[CPU_MP4HT_E0], NULL, 0, NULL },

	{ NULL, }
};
#undef _BANIAS
#undef BANIAS

static int centrino_cpu_init_table(struct cpufreq_policy *policy)
{
	struct cpuinfo_x86 *cpu = &cpu_data(policy->cpu);
	struct cpu_model *model;

	for(model = models; model->cpu_id != NULL; model++)
		if (centrino_verify_cpu_id(cpu, model->cpu_id) &&
		    (model->model_name == NULL ||
		     strcmp(cpu->x86_model_id, model->model_name) == 0))
			break;

	if (model->cpu_id == NULL) {
		/* No match at all */
		dprintk("no support for CPU model \"%s\": "
		       "send /proc/cpuinfo to " MAINTAINER "\n",
		       cpu->x86_model_id);
		return -ENOENT;
	}

	if (model->op_points == NULL) {
		/* Matched a non-match */
		dprintk("no table support for CPU model \"%s\"\n",
		       cpu->x86_model_id);
		dprintk("try using the acpi-cpufreq driver\n");
		return -ENOENT;
	}

	centrino_model[policy->cpu] = model;

	dprintk("found \"%s\": max frequency: %dkHz\n",
	       model->model_name, model->max_freq);

	return 0;
}

#else
static inline int centrino_cpu_init_table(struct cpufreq_policy *policy) { return -ENODEV; }
#endif /* CONFIG_X86_SPEEDSTEP_CENTRINO_TABLE */

static int centrino_verify_cpu_id(const struct cpuinfo_x86 *c, const struct cpu_id *x)
{
	if ((c->x86 == x->x86) &&
	    (c->x86_model == x->x86_model) &&
	    (c->x86_mask == x->x86_mask))
		return 1;
	return 0;
}

/* To be called only after centrino_model is initialized */
static unsigned extract_clock(unsigned msr, unsigned int cpu, int failsafe)
{
	int i;

	/*
	 * Extract clock in kHz from PERF_CTL value
	 * for centrino, as some DSDTs are buggy.
	 * Ideally, this can be done using the acpi_data structure.
	 */
	if ((centrino_cpu[cpu] == &cpu_ids[CPU_BANIAS]) ||
	    (centrino_cpu[cpu] == &cpu_ids[CPU_DOTHAN_A1]) ||
	    (centrino_cpu[cpu] == &cpu_ids[CPU_DOTHAN_B0])) {
		msr = (msr >> 8) & 0xff;
		return msr * 100000;
	}

	if ((!centrino_model[cpu]) || (!centrino_model[cpu]->op_points))
		return 0;

	msr &= 0xffff;
	for (i=0;centrino_model[cpu]->op_points[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (msr == centrino_model[cpu]->op_points[i].index)
			return centrino_model[cpu]->op_points[i].frequency;
	}
	if (failsafe)
		return centrino_model[cpu]->op_points[i-1].frequency;
	else
		return 0;
}

/* Return the current CPU frequency in kHz */
static unsigned int get_cur_freq(unsigned int cpu)
{
	unsigned l, h;
	unsigned clock_freq;
	cpumask_t saved_mask;

	saved_mask = current->cpus_allowed;
	set_cpus_allowed(current, cpumask_of_cpu(cpu));
	if (smp_processor_id() != cpu)
		return 0;

	rdmsr(MSR_IA32_PERF_STATUS, l, h);
	clock_freq = extract_clock(l, cpu, 0);

	if (unlikely(clock_freq == 0)) {
		/*
		 * On some CPUs, we can see transient MSR values (which are
		 * not present in _PSS), while CPU is doing some automatic
		 * P-state transition (like TM2). Get the last freq set 
		 * in PERF_CTL.
		 */
		rdmsr(MSR_IA32_PERF_CTL, l, h);
		clock_freq = extract_clock(l, cpu, 1);
	}

	set_cpus_allowed(current, saved_mask);
	return clock_freq;
}


static int centrino_cpu_init(struct cpufreq_policy *policy)
{
	struct cpuinfo_x86 *cpu = &cpu_data(policy->cpu);
	unsigned freq;
	unsigned l, h;
	int ret;
	int i;

	/* Only Intel makes Enhanced Speedstep-capable CPUs */
	if (cpu->x86_vendor != X86_VENDOR_INTEL || !cpu_has(cpu, X86_FEATURE_EST))
		return -ENODEV;

	if (cpu_has(cpu, X86_FEATURE_CONSTANT_TSC))
		centrino_driver.flags |= CPUFREQ_CONST_LOOPS;

	if (policy->cpu != 0)
		return -ENODEV;

	for (i = 0; i < N_IDS; i++)
		if (centrino_verify_cpu_id(cpu, &cpu_ids[i]))
			break;

	if (i != N_IDS)
		centrino_cpu[policy->cpu] = &cpu_ids[i];

	if (!centrino_cpu[policy->cpu]) {
		dprintk("found unsupported CPU with "
		"Enhanced SpeedStep: send /proc/cpuinfo to "
		MAINTAINER "\n");
		return -ENODEV;
	}

	if (centrino_cpu_init_table(policy)) {
		return -ENODEV;
	}

	/* Check to see if Enhanced SpeedStep is enabled, and try to
	   enable it if not. */
	rdmsr(MSR_IA32_MISC_ENABLE, l, h);

	if (!(l & (1<<16))) {
		l |= (1<<16);
		dprintk("trying to enable Enhanced SpeedStep (%x)\n", l);
		wrmsr(MSR_IA32_MISC_ENABLE, l, h);

		/* check to see if it stuck */
		rdmsr(MSR_IA32_MISC_ENABLE, l, h);
		if (!(l & (1<<16))) {
			printk(KERN_INFO PFX "couldn't enable Enhanced SpeedStep\n");
			return -ENODEV;
		}
	}

	freq = get_cur_freq(policy->cpu);

	policy->cpuinfo.transition_latency = 10000; /* 10uS transition latency */
	policy->cur = freq;

	dprintk("centrino_cpu_init: cur=%dkHz\n", policy->cur);

	ret = cpufreq_frequency_table_cpuinfo(policy, centrino_model[policy->cpu]->op_points);
	if (ret)
		return (ret);

	cpufreq_frequency_table_get_attr(centrino_model[policy->cpu]->op_points, policy->cpu);

	return 0;
}

static int centrino_cpu_exit(struct cpufreq_policy *policy)
{
	unsigned int cpu = policy->cpu;

	if (!centrino_model[cpu])
		return -ENODEV;

	cpufreq_frequency_table_put_attr(cpu);

	centrino_model[cpu] = NULL;

	return 0;
}

/**
 * centrino_verify - verifies a new CPUFreq policy
 * @policy: new policy
 *
 * Limit must be within this model's frequency range at least one
 * border included.
 */
static int centrino_verify (struct cpufreq_policy *policy)
{
	return cpufreq_frequency_table_verify(policy, centrino_model[policy->cpu]->op_points);
}

/**
 * centrino_setpolicy - set a new CPUFreq policy
 * @policy: new policy
 * @target_freq: the target frequency
 * @relation: how that frequency relates to achieved frequency (CPUFREQ_RELATION_L or CPUFREQ_RELATION_H)
 *
 * Sets a new CPUFreq policy.
 */
static int centrino_target (struct cpufreq_policy *policy,
			    unsigned int target_freq,
			    unsigned int relation)
{
	unsigned int    newstate = 0;
	unsigned int	msr, oldmsr = 0, h = 0, cpu = policy->cpu;
	struct cpufreq_freqs	freqs;
	cpumask_t		online_policy_cpus;
	cpumask_t		saved_mask;
	cpumask_t		set_mask;
	cpumask_t		covered_cpus;
	int			retval = 0;
	unsigned int		j, k, first_cpu, tmp;

	if (unlikely(centrino_model[cpu] == NULL))
		return -ENODEV;

	if (unlikely(cpufreq_frequency_table_target(policy,
			centrino_model[cpu]->op_points,
			target_freq,
			relation,
			&newstate))) {
		return -EINVAL;
	}

#ifdef CONFIG_HOTPLUG_CPU
	/* cpufreq holds the hotplug lock, so we are safe from here on */
	cpus_and(online_policy_cpus, cpu_online_map, policy->cpus);
#else
	online_policy_cpus = policy->cpus;
#endif

	saved_mask = current->cpus_allowed;
	first_cpu = 1;
	cpus_clear(covered_cpus);
	for_each_cpu_mask(j, online_policy_cpus) {
		/*
		 * Support for SMP systems.
		 * Make sure we are running on CPU that wants to change freq
		 */
		cpus_clear(set_mask);
		if (policy->shared_type == CPUFREQ_SHARED_TYPE_ANY)
			cpus_or(set_mask, set_mask, online_policy_cpus);
		else
			cpu_set(j, set_mask);

		set_cpus_allowed(current, set_mask);
		preempt_disable();
		if (unlikely(!cpu_isset(smp_processor_id(), set_mask))) {
			dprintk("couldn't limit to CPUs in this domain\n");
			retval = -EAGAIN;
			if (first_cpu) {
				/* We haven't started the transition yet. */
				goto migrate_end;
			}
			preempt_enable();
			break;
		}

		msr = centrino_model[cpu]->op_points[newstate].index;

		if (first_cpu) {
			rdmsr(MSR_IA32_PERF_CTL, oldmsr, h);
			if (msr == (oldmsr & 0xffff)) {
				dprintk("no change needed - msr was and needs "
					"to be %x\n", oldmsr);
				retval = 0;
				goto migrate_end;
			}

			freqs.old = extract_clock(oldmsr, cpu, 0);
			freqs.new = extract_clock(msr, cpu, 0);

			dprintk("target=%dkHz old=%d new=%d msr=%04x\n",
				target_freq, freqs.old, freqs.new, msr);

			for_each_cpu_mask(k, online_policy_cpus) {
				freqs.cpu = k;
				cpufreq_notify_transition(&freqs,
					CPUFREQ_PRECHANGE);
			}

			first_cpu = 0;
			/* all but 16 LSB are reserved, treat them with care */
			oldmsr &= ~0xffff;
			msr &= 0xffff;
			oldmsr |= msr;
		}

		wrmsr(MSR_IA32_PERF_CTL, oldmsr, h);
		if (policy->shared_type == CPUFREQ_SHARED_TYPE_ANY) {
			preempt_enable();
			break;
		}

		cpu_set(j, covered_cpus);
		preempt_enable();
	}

	for_each_cpu_mask(k, online_policy_cpus) {
		freqs.cpu = k;
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	}

	if (unlikely(retval)) {
		/*
		 * We have failed halfway through the frequency change.
		 * We have sent callbacks to policy->cpus and
		 * MSRs have already been written on coverd_cpus.
		 * Best effort undo..
		 */

		if (!cpus_empty(covered_cpus)) {
			for_each_cpu_mask(j, covered_cpus) {
				set_cpus_allowed(current, cpumask_of_cpu(j));
				wrmsr(MSR_IA32_PERF_CTL, oldmsr, h);
			}
		}

		tmp = freqs.new;
		freqs.new = freqs.old;
		freqs.old = tmp;
		for_each_cpu_mask(j, online_policy_cpus) {
			freqs.cpu = j;
			cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
			cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
		}
	}
	set_cpus_allowed(current, saved_mask);
	return 0;

migrate_end:
	preempt_enable();
	set_cpus_allowed(current, saved_mask);
	return 0;
}

static struct freq_attr* centrino_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver centrino_driver = {
	.name		= "centrino", /* should be speedstep-centrino,
					 but there's a 16 char limit */
	.init		= centrino_cpu_init,
	.exit		= centrino_cpu_exit,
	.verify		= centrino_verify,
	.target		= centrino_target,
	.get		= get_cur_freq,
	.attr           = centrino_attr,
	.owner		= THIS_MODULE,
};


/**
 * centrino_init - initializes the Enhanced SpeedStep CPUFreq driver
 *
 * Initializes the Enhanced SpeedStep support. Returns -ENODEV on
 * unsupported devices, -ENOENT if there's no voltage table for this
 * particular CPU model, -EINVAL on problems during initiatization,
 * and zero on success.
 *
 * This is quite picky.  Not only does the CPU have to advertise the
 * "est" flag in the cpuid capability flags, we look for a specific
 * CPU model and stepping, and we need to have the exact model name in
 * our voltage tables.  That is, be paranoid about not releasing
 * someone's valuable magic smoke.
 */
static int __init centrino_init(void)
{
	struct cpuinfo_x86 *cpu = &cpu_data(0);

	if (!cpu_has(cpu, X86_FEATURE_EST))
		return -ENODEV;

	return cpufreq_register_driver(&centrino_driver);
}

static void __exit centrino_exit(void)
{
	cpufreq_unregister_driver(&centrino_driver);
}

MODULE_AUTHOR ("Jeremy Fitzhardinge <jeremy@goop.org>");
MODULE_DESCRIPTION ("Enhanced SpeedStep driver for Intel Pentium M processors.");
MODULE_LICENSE ("GPL");

late_initcall(centrino_init);
module_exit(centrino_exit);
