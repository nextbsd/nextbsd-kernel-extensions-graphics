/*-
 * SPDX-License-Identifier: MIT
 *
 * dev.v3d.0.state -- read what the GPU and its schedulers are actually doing.
 *
 * The Pi wedges intermittently: v3d interrupt counters stop, a job's fence
 * never signals, X blocks forever in an atomic commit, and nothing is logged
 * because drm_sched's timeout handler is never reached. Telling the possible
 * causes apart needs the state at the moment it is stuck, and v3d_debugfs.c is
 * not vendored here (CONFIG_DEBUG_FS is off, same as vc4_kms), so read it out
 * directly.
 *
 * This lives in its own file on purpose. v3d_newbus.c handles newbus
 * resources and therefore sees FreeBSD's struct resource from <sys/rman.h>;
 * v3d_drv.h drags in <linux/ioport.h>, which declares a different struct of
 * the same name. Keeping the two apart is the rule in the linuxkpi-changes
 * skill, and mixing them is what broke i915 and amdgpu before.
 *
 * What the fields mean:
 *   pending      NONEMPTY means a job is queued with the scheduler, so
 *                drm_sched_start_timeout() should have armed the TDR. Empty
 *                while X waits on a fence means the job was never submitted --
 *                a very different bug from the GPU hanging.
 *   active       the job the queue believes is on the hardware.
 *   CTnCA/CTnRA  CLE addresses. Sample twice: moving means the GPU is looping
 *                in a control list, static means it is idle.
 *   INT_STS      a set bit with a frozen interrupt counter means the GPU
 *                raised a completion the CPU never took -- delivery, not
 *                execution.
 *   ERR_STAT     non-zero means the GPU faulted.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/queue.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/kernel.h>

#include <linux/device.h>
#include <linux/workqueue.h>

#include <drm/gpu_scheduler.h>
#include <linux/dma-fence.h>
#include <drm/spsc_queue.h>

#include "v3d_drv.h"
#include "v3d_regs.h"	/* register offsets: v3d_drv.h does not pull these in */

extern unsigned long v3d_wd_kicks;
extern unsigned long v3d_wd_depkicks;
extern unsigned long v3d_wd_wakekicks;

int v3d_sysctl_state(SYSCTL_HANDLER_ARGS);

int
v3d_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	struct device *dev = arg1;
	struct drm_device *drm;
	struct v3d_dev *v3d;
	struct sbuf sb;
	int error, q, i;

	if (dev == NULL)
		return (ENXIO);
	drm = dev_get_drvdata(dev);
	if (drm == NULL)
		return (ENXIO);
	v3d = to_v3d_dev(drm);

	sbuf_new_for_sysctl(&sb, NULL, 1024, req);

	sbuf_printf(&sb, "\n");
	sbuf_printf(&sb, "hub  INT_STS 0x%08x\n", V3D_READ(V3D_HUB_INT_STS));
	sbuf_printf(&sb, "core INT_STS 0x%08x\n",
	    V3D_CORE_READ(0, V3D_CTL_INT_STS));
	sbuf_printf(&sb, "ERR_STAT     0x%08x\n",
	    V3D_CORE_READ(0, V3D_ERR_STAT));
	sbuf_printf(&sb, "watchdog kicks %lu depkicks %lu wakekicks %lu\n",
	    v3d_wd_kicks, v3d_wd_depkicks, v3d_wd_wakekicks);

	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		struct v3d_queue_state *qs = &v3d->queue[q];

		uint64_t done = qs->stats.jobs_completed;

		/*
		 * emit vs done is the number that matters. emit_seqno counts
		 * jobs whose fence has been created (and attached to the BO's
		 * dma_resv); jobs_completed counts those that finished. A gap
		 * with pending=empty means the jobs are sitting UNSUBMITTED in
		 * the scheduler's entity queue -- drm_sched only moves a job
		 * to pending_list once its submit worker pushes it to the
		 * hardware. That state produces exactly what we see: no
		 * interrupts, no armed timeout (it only arms for pending
		 * jobs), no messages, and a fence that never signals.
		 */
		sbuf_printf(&sb,
		    "q%d emit=%llu done=%llu outstanding=%lld active=%-3s pending=%s",
		    q, (unsigned long long)qs->emit_seqno,
		    (unsigned long long)done,
		    (long long)(qs->emit_seqno - done),
		    qs->active_job != NULL ? "yes" : "no",
		    list_empty(&qs->sched.pending_list) ? "empty" : "NONEMPTY");
		sbuf_printf(&sb, " credit=%d ready=%d paused=%d",
		    atomic_read(&qs->sched.credit_count),
		    qs->sched.ready, qs->sched.pause_submit);

		/*
		 * The state of the scheduler's own submit work item. This is
		 * THE discriminator for the captured wedge, where the render
		 * queue sits at jobs=107 dep=0 stop=0 credit=0 ready=1
		 * paused=0 -- i.e. drm_sched_select_entity() would return that
		 * entity at once, so the only remaining explanation is that
		 * drm_sched_run_job_work() is never invoked.
		 *
		 * LinuxKPI work states (linux_work.c):
		 *   0 IDLE   not queued, not running
		 *   1 TIMER  delayed work timer pending
		 *   2 TASK   queued on the taskqueue
		 *   3 EXEC   callback executing -- note this is also the normal
		 *             RESTING state: linux_work_fn() exits its loop by
		 *             falling through to "goto done" without resetting
		 *             to IDLE, so EXEC on an idle queue is expected and
		 *             is NOT evidence of anything.
		 *   4 CANCEL cancel requested
		 *
		 * With jobs waiting:
		 *   IDLE -> the queue_work() wakeup was dropped; drm_sched is
		 *           correct and LinuxKPI lost it. drm_sched_run_job_work()
		 *           re-queues itself only while running, so one lost
		 *           wakeup stalls the queue permanently.
		 *   TASK -> claims to be queued. The second number, ta_pending,
		 *           is what makes this conclusive: it is the underlying
		 *           struct task's pending count, so
		 *             TASK with ta_pending=0  => NOT on any taskqueue.
		 *           The only path in linux_queue_work_on() that sets
		 *           TASK without calling taskqueue_enqueue() is the
		 *           linux_work_exec_unblock() shortcut, which returns
		 *           true and relies on the running linux_work_fn loop
		 *           to re-run the callback.
		 *             TASK with ta_pending>0  => really queued, and the
		 *           taskqueue thread is at fault instead.
		 */
		sbuf_printf(&sb, " runwork=%d/%d freework=%d/%d",
		    atomic_read(&qs->sched.work_run_job.state),
		    qs->sched.work_run_job.work_task.ta_pending,
		    atomic_read(&qs->sched.work_free_job.state),
		    qs->sched.work_free_job.work_task.ta_pending);

		/*
		 * Walk the run-queues.
		 *
		 * The captured wedge shows an UNSIGNALLED drm_sched fence whose
		 * seqno is ahead of anything v3d has emitted to hardware: the
		 * job was armed (fence created and attached to the BOs, so
		 * every waiter blocks) but never submitted. pending_list is
		 * therefore empty, no interrupt fires, and drm_sched arms no
		 * timeout -- which is why the hang is completely silent.
		 *
		 * drm_sched_run_job_work() returns WITHOUT re-queueing itself
		 * when drm_sched_select_entity() yields NULL, so one entity
		 * holding jobs while invisible to the run-queue stalls the
		 * scheduler forever.
		 *
		 * "rq[i] ents=N jobs=M" is the discriminator: jobs>0 with the
		 * scheduler idle means they are queued and not being picked up.
		 * ents=0 while a fence is unsignalled means the entity fell out
		 * of the run-queue entirely.
		 */
		for (i = 0; i < qs->sched.num_rqs; i++) {
			struct drm_sched_rq *rq = qs->sched.sched_rq[i];
			struct drm_sched_entity *ent;
			int nents = 0;
			unsigned int njobs = 0;
			int ndep = 0, nstop = 0;

			if (rq == NULL)
				continue;
			spin_lock(&rq->lock);
			list_for_each_entry(ent, &rq->entities, list) {
				struct dma_fence *d = ent->dependency;

				nents++;
				njobs += spsc_queue_count(&ent->job_queue);
				if (ent->stopped)
					nstop++;
				if (d == NULL)
					continue;
				ndep++;
				/*
				 * Name the dependency. An entity parked on an
				 * unsignalled dependency is drm_sched working
				 * as designed -- drm_sched_entity_pop_job()
				 * registers a callback and returns NULL, and
				 * the scheduler waits for drm_sched_wakeup().
				 * But if that fence never signals, or its
				 * callback is lost, the entity stalls forever
				 * with jobs piling up behind it, and nothing
				 * is wrongly queued so the taskqueue watchdog
				 * cannot see it.
				 *
				 * signalled=1 here means the fence DID fire and
				 * the wakeup callback was lost -- a different
				 * bug from a fence that never fires, and a
				 * different fix.
				 */
				sbuf_printf(&sb,
				    "\n    q%d dep fence %s/%s ctx %llu seqno %llu signalled=%d",
				    q,
				    (d->ops != NULL &&
				     d->ops->get_driver_name != NULL) ?
				    d->ops->get_driver_name(d) : "?",
				    (d->ops != NULL &&
				     d->ops->get_timeline_name != NULL) ?
				    d->ops->get_timeline_name(d) : "?",
				    (unsigned long long)d->context,
				    (unsigned long long)d->seqno,
				    dma_fence_is_signaled(d));
			}
			spin_unlock(&rq->lock);
			if (nents != 0 || njobs != 0)
				sbuf_printf(&sb,
				    " rq[%d]{ents=%d jobs=%u dep=%d stop=%d}",
				    i, nents, njobs, ndep, nstop);
		}
		if (q == V3D_BIN || q == V3D_RENDER)
			sbuf_printf(&sb, " CTnCA=0x%08x CTnRA=0x%08x",
			    V3D_CORE_READ(0, V3D_CLE_CTNCA(q)),
			    V3D_CORE_READ(0, V3D_CLE_CTNRA(q)));
		sbuf_printf(&sb, "\n");
	}

	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

/*
 * dev.v3d.0.kick -- re-kick the scheduler submit taskqueues.
 *
 * The wedge is: drm_sched's submit work item sits on its taskqueue with
 * ta_pending=1 while the dedicated thread is idle -- i.e. the work is
 * genuinely queued and the wakeup that should have dispatched it was lost.
 * It is self-perpetuating, because taskqueue_enqueue_locked() only kicks the
 * thread on the 0->1 transition:
 *
 *	if (task->ta_pending) { task->ta_pending++; TQ_UNLOCK(queue); return; }
 *
 * so every later queue_work() bumps a counter and wakes nothing.
 *
 * taskqueue_unblock() is the one exported call that breaks that: it clears
 * TQ_FLAGS_BLOCKED *and* re-issues tq_enqueue() when the queue is non-empty.
 * So writing 1 here distinguishes the two remaining candidates, and does it
 * without patching the kernel:
 *
 *   desktop recovers  the task really was queued with no one coming to run it
 *                     -- a lost wakeup (or a queue left blocked). This is then
 *                     also a usable recovery mechanism.
 *   nothing happens   the task is not actually dispatchable and the fault is
 *                     elsewhere.
 */
int v3d_sysctl_kick(SYSCTL_HANDLER_ARGS);

int
v3d_sysctl_kick(SYSCTL_HANDLER_ARGS)
{
	struct device *dev = arg1;
	struct drm_device *drm;
	struct v3d_dev *v3d;
	int error, val, q;

	val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val == 0)
		return (0);

	if (dev == NULL)
		return (ENXIO);
	drm = dev_get_drvdata(dev);
	if (drm == NULL)
		return (ENXIO);
	v3d = to_v3d_dev(drm);

	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		struct v3d_queue_state *qs = &v3d->queue[q];
		struct workqueue_struct *wq = qs->sched.submit_wq;

		printf("V3DKICK: q%d runwork=%d/%d freework=%d/%d wq=%p tq=%p\n",
		    q,
		    atomic_read(&qs->sched.work_run_job.state),
		    qs->sched.work_run_job.work_task.ta_pending,
		    atomic_read(&qs->sched.work_free_job.state),
		    qs->sched.work_free_job.work_task.ta_pending,
		    wq, (wq != NULL) ? wq->taskqueue : NULL);

		if (wq != NULL && wq->taskqueue != NULL)
			taskqueue_unblock(wq->taskqueue);
	}
	printf("V3DKICK: unblocked all submit taskqueues\n");

	return (0);
}

/*
 * Watchdog: a safety net for a scheduler that has stopped making progress
 * (nextbsd#450, nextbsd-kernel-extensions-graphics#124).
 *
 * WHAT IT USED TO DO, AND WHY THAT WAS THE BUG IN #124
 *
 * The first version looked at one 50ms snapshot and repaired whatever looked
 * wrong in it:
 *
 *   case A  run work TASK with ta_pending > 0, twice in a row -> unblock
 *   case B  an entity whose dependency fence has signalled -> remove the
 *           entity's callback, clear entity->dependency, put the fence
 *   case C  run work TASK with ta_pending == 0 and no executor -> enqueue
 *
 * Every one of those states is also one the scheduler passes through
 * legitimately, for a few microseconds, many times a second:
 *
 *   B  dma_fence_signal() sets the SIGNALED bit before it runs the callbacks,
 *      and drm_sched_entity_pop_job() leaves an already-signalled fence in
 *      entity->dependency for a moment while it moves on to the next one.
 *   C  between linux_queue_work_on()'s EXEC->TASK transition and its
 *      taskqueue_enqueue(), and between the taskqueue thread dequeueing the
 *      task (ta_pending = 0) and linux_work_fn() registering its executor.
 *   A  two separate enqueue episodes 50ms apart look like one stuck one.
 *
 * Instrumented on a Pi 500+ with WindowServer + InnerSpace GL + wsgldemo, with
 * the repairs switched off: every case B and case C sighting cleared by
 * itself, without the watchdog, and the queues never stalled. The "lost
 * callbacks" and "lost hand-offs" it counted were the watchdog sampling those
 * windows, not bugs in LinuxKPI.
 *
 * For C and A the old repair was harmless (an extra run of an idempotent work
 * item). For B it was not. entity->dependency and entity->cb are owned by the
 * scheduler thread and the fence's signal path; rq->lock protects neither. If
 * the callout read entity->dependency (fence X, signalled) just before
 * drm_sched_entity_pop_job() moved the entity on to its next dependency Y and
 * registered entity->cb on Y, then
 *
 *	dma_fence_remove_callback(X, &ent->cb)
 *
 * found the callback non-empty -- it is on Y's list -- and unlinked it from Y's
 * list while holding X's lock, not Y's; cleared entity->dependency although Y
 * had NOT signalled, so the job ran before its dependency; and put X a second
 * time, freeing a scheduler fence that its job still used. Seen on the hardware
 * as dma_fence_remove_callback() returning true for a signalled fence, which a
 * correct dma_fence makes impossible unless the callback is on another fence.
 * Running a v3d job before its dependency, and a use-after-free of a
 * drm_sched_fence, are exactly the #124 symptoms: the PTB MMU fault, the
 * wedged GPU, and the spontaneous reboots.
 *
 * WHAT IT DOES NOW
 *
 * Nothing, unless a queue has demonstrably STOPPED: jobs are waiting in its
 * run-queues, nothing is on the hardware, and for V3D_WD_STALL_POLLS polls in
 * a row (500ms -- five orders of magnitude longer than any of the windows
 * above) neither the queue's emit_seqno nor its waiting-job count has moved.
 * Only then does it look for one of the three broken states, and the repair for
 * B is made safe by that freeze: the entity cannot be selected while its
 * dependency is set, so drm_sched_entity_pop_job() is not running for it, and
 * the fence lock is held while checking that entity->dependency is still the
 * signalled fence and that entity->cb is on no list at all. If the callback is
 * on some list, the state is inconsistent and is reported, never touched.
 *
 * A healthy system never trips it, so any kick in dev.v3d.0.state is a real
 * stall worth reporting. Every repair is printed.
 */
#define	V3D_WORK_ST_TASK	2	/* enum is private to linux_work.c */

#define	V3D_WD_HZ		20	/* poll every 50ms */
#define	V3D_WD_STALL_POLLS	10	/* ...and act after 500ms with no progress */

static struct callout	v3d_wd_callout;
static struct device   *v3d_wd_dev;
unsigned long		v3d_wd_kicks;		/* A: queued task not dispatched */
unsigned long		v3d_wd_depkicks;	/* B: signalled dependency kept */
unsigned long		v3d_wd_wakekicks;	/* C: TASK with nothing queued */

static struct v3d_wd_queue {
	u64		emit;		/* emit_seqno at the previous poll */
	unsigned int	njobs;		/* jobs waiting at the previous poll */
	int		stalled;	/* consecutive polls without progress */
} v3d_wd_q[V3D_MAX_QUEUES];

/* Jobs waiting in the queue's run-queues, not yet handed to the hardware. */
static unsigned int
v3d_wd_waiting(struct drm_gpu_scheduler *sched)
{
	struct drm_sched_entity *ent;
	unsigned int njobs = 0;
	int i;

	for (i = 0; i < sched->num_rqs; i++) {
		struct drm_sched_rq *rq = sched->sched_rq[i];

		if (rq == NULL)
			continue;
		spin_lock(&rq->lock);
		list_for_each_entry(ent, &rq->entities, list)
			njobs += spsc_queue_count(&ent->job_queue);
		spin_unlock(&rq->lock);
	}
	return (njobs);
}

/*
 * Case B, on a frozen queue only: an entity still parked on a dependency that
 * has signalled. Returns true if it cleared one.
 */
static bool
v3d_wd_clear_signalled_dep(int q, struct drm_gpu_scheduler *sched)
{
	struct drm_sched_entity *ent;
	bool cleared = false;
	int i;

	for (i = 0; i < sched->num_rqs; i++) {
		struct drm_sched_rq *rq = sched->sched_rq[i];

		if (rq == NULL)
			continue;
		spin_lock(&rq->lock);
		list_for_each_entry(ent, &rq->entities, list) {
			struct dma_fence *f = READ_ONCE(ent->dependency);
			bool clear = false, onlist = false;

			if (f == NULL || !dma_fence_is_signaled(f))
				continue;

			/*
			 * Under the fence's own lock: the signal path runs the
			 * callbacks with it held, so nothing can be half-way
			 * through calling ours.
			 */
			spin_lock(f->lock);
			if (READ_ONCE(ent->dependency) == f) {
				onlist = !list_empty(&ent->cb.node);
				if (!onlist) {
					WRITE_ONCE(ent->dependency, NULL);
					clear = true;
				}
			}
			spin_unlock(f->lock);

			if (onlist) {
				printf("V3DWD: q%d entity parked on signalled "
				    "%s ctx %llu seqno %llu with its callback "
				    "on another fence -- inconsistent, not "
				    "touching it\n", q,
				    (f->ops != NULL &&
				     f->ops->get_timeline_name != NULL) ?
				    f->ops->get_timeline_name(f) : "?",
				    (unsigned long long)f->context,
				    (unsigned long long)f->seqno);
				continue;
			}
			if (!clear)
				continue;

			/* The reference the lost callback would have put. */
			dma_fence_put(f);
			v3d_wd_depkicks++;
			cleared = true;
			printf("V3DWD: q%d stalled 500ms on an already-signalled "
			    "dependency -- its callback never ran; cleared "
			    "(dep kick #%lu)\n", q, v3d_wd_depkicks);
		}
		spin_unlock(&rq->lock);
	}
	return (cleared);
}

static void
v3d_watchdog(void *arg __unused)
{
	struct drm_device *drm;
	struct v3d_dev *v3d;
	int q;

	if (v3d_wd_dev == NULL)
		goto rearm;
	drm = dev_get_drvdata(v3d_wd_dev);
	if (drm == NULL)
		goto rearm;
	v3d = to_v3d_dev(drm);

	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		struct v3d_queue_state *qs = &v3d->queue[q];
		struct drm_gpu_scheduler *sched = &qs->sched;
		struct v3d_wd_queue *wq_state = &v3d_wd_q[q];
		struct workqueue_struct *wq = sched->submit_wq;
		struct work_struct *w = &sched->work_run_job;
		unsigned int njobs;
		u64 emit;
		bool repaired = false;
		int state, pending;

		if (!sched->ready || wq == NULL || wq->taskqueue == NULL)
			continue;

		emit = READ_ONCE(qs->emit_seqno);
		njobs = v3d_wd_waiting(sched);

		/*
		 * Progress, or nothing to make progress on, or a job on the
		 * hardware (that is drm_sched's own timeout's business).
		 */
		if (njobs == 0 || emit != wq_state->emit ||
		    njobs != wq_state->njobs ||
		    !list_empty(&sched->pending_list) ||
		    READ_ONCE(sched->pause_submit)) {
			wq_state->emit = emit;
			wq_state->njobs = njobs;
			wq_state->stalled = 0;
			continue;
		}
		if (++wq_state->stalled < V3D_WD_STALL_POLLS)
			continue;
		wq_state->stalled = 0;

		/* B: a dependency that signalled without waking the entity. */
		if (v3d_wd_clear_signalled_dep(q, sched))
			repaired = true;

		state = atomic_read(&w->state);
		pending = w->work_task.ta_pending;

		if (state == V3D_WORK_ST_TASK && pending == 0) {
			/* C: claims to be queued, is on no taskqueue. */
			struct work_exec *ex;
			bool executing = false;

			mtx_lock(&wq->exec_mtx);
			TAILQ_FOREACH(ex, &wq->exec_head, entry) {
				if (ex->target == w) {
					executing = true;
					break;
				}
			}
			mtx_unlock(&wq->exec_mtx);

			if (!executing) {
				v3d_wd_wakekicks++;
				printf("V3DWD: q%d stalled 500ms with its submit "
				    "work in WORK_ST_TASK on no taskqueue and no "
				    "executor -- re-enqueueing (wake kick #%lu)\n",
				    q, v3d_wd_wakekicks);
				taskqueue_enqueue(wq->taskqueue, &w->work_task);
				repaired = true;
			}
		} else if (state == V3D_WORK_ST_TASK && pending > 0) {
			/* A: genuinely queued and never dispatched. */
			v3d_wd_kicks++;
			printf("V3DWD: q%d stalled 500ms with its submit work "
			    "queued (ta_pending=%d) and not dispatched -- "
			    "unblocking the taskqueue (kick #%lu)\n", q,
			    pending, v3d_wd_kicks);
			taskqueue_unblock(wq->taskqueue);
			repaired = true;
		}

		/*
		 * Anything else is a queue waiting on a dependency that has not
		 * signalled yet, which is the scheduler working as designed.
		 */
		if (repaired)
			drm_sched_wakeup(sched);
	}

rearm:
	callout_reset(&v3d_wd_callout, hz / V3D_WD_HZ, v3d_watchdog, NULL);
}

void v3d_watchdog_start(struct device *dev);
void v3d_watchdog_stop(void);

void
v3d_watchdog_start(struct device *dev)
{
	v3d_wd_dev = dev;
	memset(v3d_wd_q, 0, sizeof(v3d_wd_q));
	callout_init(&v3d_wd_callout, 1);
	callout_reset(&v3d_wd_callout, hz / V3D_WD_HZ, v3d_watchdog, NULL);
}

void
v3d_watchdog_stop(void)
{
	v3d_wd_dev = NULL;
	callout_drain(&v3d_wd_callout);
}
