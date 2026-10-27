/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The component framework, in the subset a single-device driver needs
 * (nextbsd-kernel#176). See linux/component.h for what is deliberately absent.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>

#include <linux/component.h>
#include <linux/device.h>
#include <linux/errno.h>

struct lkpi_component {
	TAILQ_ENTRY(lkpi_component)	link;
	struct device			*dev;
	const struct component_ops	*ops;
	bool				bound;
	/*
	 * Which master this component is bound to, and the data it was bound
	 * with. Recorded at bind because component_del() has to be able to
	 * unbind, and unbind callbacks read the master: vc4_hvs_unbind() calls
	 * dev_get_drvdata(master) as its first statement, so passing NULL
	 * there is a panic on kldunload, not a degraded teardown.
	 */
	struct device		*master;
	void			*master_data;
};

/*
 * Named rather than anonymous: TAILQ_FOREACH_REVERSE() needs the head type by
 * name, and unbind walks backwards.
 */
static TAILQ_HEAD(lkpi_component_head, lkpi_component) lkpi_components =
    TAILQ_HEAD_INITIALIZER(lkpi_components);
static struct mtx lkpi_component_mtx;
MTX_SYSINIT(lkpi_component, &lkpi_component_mtx, "lkpi component", MTX_DEF);

/* Defined with the match code below; called when a component appears. */
static void lkpi_try_bind_masters(void);

/*
 * One entry in a master's match list: a predicate and its argument. The
 * component it selects is resolved at bind time, not here, because components
 * may register after the master builds its list.
 */
struct lkpi_match_entry {
	TAILQ_ENTRY(lkpi_match_entry)	link;
	int			      (*compare)(struct device *, void *);
	void			       *data;
};

struct component_match {
	TAILQ_HEAD(lkpi_match_head, lkpi_match_entry)	entries;
	unsigned int			count;
};

/* A registered master, waiting for its components to appear. */
struct lkpi_master {
	TAILQ_ENTRY(lkpi_master)	   link;
	struct device			  *dev;
	const struct component_master_ops *ops;
	struct component_match		  *match;
	bool				   bound;
};

static TAILQ_HEAD(, lkpi_master) lkpi_masters =
    TAILQ_HEAD_INITIALIZER(lkpi_masters);


int
component_add(struct device *dev, const struct component_ops *ops)
{
	struct lkpi_component *c;

	if (dev == NULL || ops == NULL || ops->bind == NULL)
		return (-EINVAL);

	c = malloc(sizeof(*c), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (c == NULL)
		return (-ENOMEM);
	c->dev = dev;
	c->ops = ops;

	mtx_lock(&lkpi_component_mtx);
	TAILQ_INSERT_TAIL(&lkpi_components, c, link);
	mtx_unlock(&lkpi_component_mtx);

	/*
	 * A master may already be waiting on exactly this component (#51).
	 * Retrying here is what makes probe order irrelevant -- master first or
	 * component first both end up bound.
	 */
	lkpi_try_bind_masters();
	return (0);
}

void
component_del(struct device *dev, const struct component_ops *ops)
{
	struct lkpi_component *c, *tmp;

	mtx_lock(&lkpi_component_mtx);
	TAILQ_FOREACH_SAFE(c, &lkpi_components, link, tmp) {
		if (c->dev != dev || c->ops != ops)
			continue;
		TAILQ_REMOVE(&lkpi_components, c, link);
		mtx_unlock(&lkpi_component_mtx);
		if (c->bound && c->ops->unbind != NULL)
			c->ops->unbind(c->dev, c->master, c->master_data);
		free(c, M_DEVBUF);
		return;
	}
	mtx_unlock(&lkpi_component_mtx);
}

/*
 * Bind every registered component to this master.
 *
 * The list is walked without the lock held across the callback, because bind
 * routines allocate, sleep and register DRM objects. Components are added
 * from probe and removed from detach, neither of which overlaps a master
 * binding in the single-device case this serves; the lock is only here to
 * keep list manipulation itself sound.
 *
 * A component that fails is left unbound and the error is returned
 * immediately, which matches Linux -- the master is expected to tear down.
 */
/*
 * Find the master registered for this device, if it has one, so bind order can
 * follow its match list. Caller must hold nothing; the master list is only
 * mutated under lkpi_component_mtx and this is a read.
 */
static struct lkpi_master *
lkpi_master_for(struct device *dev)
{
	struct lkpi_master *m;

	TAILQ_FOREACH(m, &lkpi_masters, link) {
		if (m->dev == dev)
			return (m);
	}
	return (NULL);
}

/*
 * Bind this master's components.
 *
 * ORDER MATTERS, and it must follow the master's MATCH LIST rather than the
 * order components happened to register. Upstream Linux does the same --
 * drivers/base/component.c indexes the match array with the comment "Bind
 * components in match order" -- and vc4 depends on it.
 *
 * Concretely, from the vc4 port (nextbsd-kernel-extensions-graphics#51):
 * vc4_crtc_bind() calls vc4_set_crtc_possible_masks(), which walks the
 * encoders ALREADY REGISTERED on the drm_device and sets
 * encoder->possible_crtcs. If a pixelvalve (CRTC) binds before the HDMI
 * encoders exist, that walk finds nothing, nothing revisits it, and the HDMI
 * encoders keep possible_crtcs == 0 -- DRM then has no CRTC able to drive the
 * HDMI connectors, fbdev finds no usable CRTC, and every modeset fails. The
 * driver loads "successfully" and the display is silently dead.
 *
 * That is not hypothetical on a Pi 5: bcm2712's device tree lists the
 * pixelvalves (0x7c410000, 0x7c411000) before the HDMI controllers
 * (0x7ef00700, 0x7ef05700), so registration order puts both CRTCs ahead of
 * both encoders on every boot.
 *
 * A master with no match list keeps the old behaviour -- bind everything in
 * registration order -- which is the single-component case this framework was
 * originally written for.
 */
int
component_bind_all(struct device *master, void *master_data)
{
	struct lkpi_master *m;
	struct lkpi_match_entry *e;
	struct lkpi_component *c;
	int error;

	m = lkpi_master_for(master);
	if (m == NULL || m->match == NULL) {
		/* No match list: legacy path, registration order. */
		TAILQ_FOREACH(c, &lkpi_components, link) {
			if (c->bound)
				continue;
			error = c->ops->bind(c->dev, master, master_data);
			if (error != 0)
				return (error);
			c->bound = true;
			c->master = master;
			c->master_data = master_data;
		}
		return (0);
	}

	TAILQ_FOREACH(e, &m->match->entries, link) {
		TAILQ_FOREACH(c, &lkpi_components, link) {
			if (c->bound || !e->compare(c->dev, e->data))
				continue;
			error = c->ops->bind(c->dev, master, master_data);
			if (error != 0)
				return (error);
			c->bound = true;
			c->master = master;
			c->master_data = master_data;
			break;		/* one component per match entry */
		}
	}
	return (0);
}

/*
 * The mirror of component_bind_all(): unbind in reverse of the order used to
 * bind. Walking registration order backwards would tear down in an order
 * unrelated to how things were brought up once a match list is in play.
 */
void
component_unbind_all(struct device *master, void *master_data)
{
	struct lkpi_master *m;
	struct lkpi_match_entry *e;
	struct lkpi_component *c;

	m = lkpi_master_for(master);
	if (m != NULL && m->match != NULL) {
		TAILQ_FOREACH_REVERSE(e, &m->match->entries,
		    lkpi_match_head, link) {
			TAILQ_FOREACH(c, &lkpi_components, link) {
				if (!c->bound || !e->compare(c->dev, e->data))
					continue;
				if (c->ops->unbind != NULL)
					c->ops->unbind(c->dev, master,
					    master_data);
				c->bound = false;
				c->master = NULL;
				c->master_data = NULL;
				break;
			}
		}
		return;
	}

	TAILQ_FOREACH_REVERSE(c, &lkpi_components, lkpi_component_head, link) {
		if (!c->bound)
			continue;
		if (c->ops->unbind != NULL)
			c->ops->unbind(c->dev, master, master_data);
		c->bound = false;
		c->master = NULL;
		c->master_data = NULL;
	}
}

/* ---- match-based masters (nextbsd-kernel-extensions-graphics#51) ---------------- */

int
component_compare_dev(struct device *dev, void *data)
{

	return (dev == (struct device *)data);
}

void
component_match_add(struct device *master __unused,
    struct component_match **match, int (*compare)(struct device *, void *),
    void *data)
{
	struct lkpi_match_entry *e;

	if (match == NULL || compare == NULL)
		return;
	if (*match == NULL) {
		*match = malloc(sizeof(**match), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (*match == NULL)
			return;
		TAILQ_INIT(&(*match)->entries);
	}
	e = malloc(sizeof(*e), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (e == NULL)
		return;
	e->compare = compare;
	e->data = data;
	TAILQ_INSERT_TAIL(&(*match)->entries, e, link);
	(*match)->count++;
}

/*
 * Can every entry in `match` be satisfied by a registered component?
 *
 * Linux defers a master until its whole match list is present, which is what
 * makes probe order irrelevant. Same here: an incomplete list means "not
 * yet", not "failed".
 */
static bool
lkpi_match_complete(struct component_match *match)
{
	struct lkpi_match_entry *e;
	struct lkpi_component *c;
	bool found;

	if (match == NULL)
		return (true);		/* no constraints: bind immediately */

	TAILQ_FOREACH(e, &match->entries, link) {
		found = false;
		TAILQ_FOREACH(c, &lkpi_components, link) {
			if (e->compare(c->dev, e->data)) {
				found = true;
				break;
			}
		}
		if (!found)
			return (false);
	}
	return (true);
}

/*
 * Try to bring up any master whose match list is now complete. Called after a
 * master registers and after a component appears, so either order works.
 */
static void
lkpi_try_bind_masters(void)
{
	struct lkpi_master *m;
	int error;

	TAILQ_FOREACH(m, &lkpi_masters, link) {
		if (m->bound || m->ops->bind == NULL)
			continue;
		if (!lkpi_match_complete(m->match))
			continue;
		error = m->ops->bind(m->dev);
		if (error != 0) {
			printf("lkpi component: master bind failed: %d\n",
			    error);
			continue;
		}
		m->bound = true;
	}
}

int
component_master_add_with_match(struct device *master,
    const struct component_master_ops *ops, struct component_match *match)
{
	struct lkpi_master *m;

	if (master == NULL || ops == NULL)
		return (-EINVAL);

	/*
	 * An empty match list is COMPLETE, so the master would bind
	 * immediately with zero components, register a drm_device with nothing
	 * attached, and report success -- a dark screen and a clean log. That
	 * is the single most misleading failure this framework can produce, and
	 * it has already been reached once by attaching the master in an
	 * earlier newbus pass than its components (#51).
	 *
	 * A driver that genuinely wants no components calls
	 * component_master_add() and never gets here, so an empty list arriving
	 * on this path always means the caller expected to find some. Say so
	 * loudly rather than binding an empty aggregate.
	 */
	if (match == NULL || match->count == 0) {
		printf("lkpi: %s: component master registered with an EMPTY "
		    "match list -- no components will bind. Attach order or "
		    "the platform-device registry is wrong (#51).\n",
		    dev_name(master) != NULL ? dev_name(master) : "?");
	}

	m = malloc(sizeof(*m), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (m == NULL)
		return (-ENOMEM);
	m->dev = master;
	m->ops = ops;
	m->match = match;

	mtx_lock(&lkpi_component_mtx);
	TAILQ_INSERT_TAIL(&lkpi_masters, m, link);
	mtx_unlock(&lkpi_component_mtx);

	/*
	 * Linux returns 0 whether or not the master bound -- an incomplete
	 * match list is "wait", not an error. Reporting a bind failure here
	 * would make a driver tear down a master that is merely early.
	 */
	lkpi_try_bind_masters();
	return (0);
}

void
component_master_del(struct device *master, const struct component_master_ops *ops)
{
	struct lkpi_master *m, *tmp;
	struct lkpi_match_entry *e, *etmp;

	mtx_lock(&lkpi_component_mtx);
	TAILQ_FOREACH_SAFE(m, &lkpi_masters, link, tmp) {
		if (m->dev != master || m->ops != ops)
			continue;
		TAILQ_REMOVE(&lkpi_masters, m, link);
		mtx_unlock(&lkpi_component_mtx);

		if (m->bound && m->ops->unbind != NULL)
			m->ops->unbind(m->dev);
		if (m->match != NULL) {
			TAILQ_FOREACH_SAFE(e, &m->match->entries, link, etmp) {
				TAILQ_REMOVE(&m->match->entries, e, link);
				free(e, M_DEVBUF);
			}
			free(m->match, M_DEVBUF);
		}
		free(m, M_DEVBUF);
		return;
	}
	mtx_unlock(&lkpi_component_mtx);
}
