#include "mango/overview/overview.h"
#include "mango/animation/client.h"
#include "mango/common/input-event-codes.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/dispatch/bind.h"
#include "mango/draw/text-node.h"
#include "mango/input/pointer.h"
#include "mango/layout/arrange.h"
#include "mango/layout/layout.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include "mango/manage/tab.h"
#include <float.h>
#include <math.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

/* Overview card surface node: each surface (including subsurfaces) maps to a
 * scene_surface node in the card tree; sx/sy are its coordinates relative to
 * the root surface. */
struct ov_card_surface {
	Client *c;
	struct wlr_surface *surface;
	struct wlr_scene_surface *scene_surface;
	struct wlr_scene_buffer *buffer;
	int sx, sy; /* Coordinates relative to the root surface */
	bool is_root;
	struct wl_list link;
	struct wl_listener commit; /* Recomputes scaling after commit (commit resets
								  dest/source). */
	struct wl_listener
		destroy; /* Removes the node when the surface is destroyed. */
};

enum {
	OV_DRAG_NONE = 0,
	OV_DRAG_MOVE_CLIENT,
	OV_DRAG_RESIZE_CLIENT,
	OV_DRAG_PAN_CANVAS,
	OV_DRAG_CLICK_ONLY,
};

struct ov_canvas_state {
	Monitor *mon;
	float cam_x;
	float cam_y;
	float cam_zoom;
	float target_cam_x;
	float target_cam_y;
	float target_cam_zoom;
	float anchor_wx;
	float anchor_wy;
	float anchor_rx;
	float anchor_ry;
	bool zoom_use_anchor;
	bool camera_animating;
	struct wl_event_source *zoom_timer;
	bool custom_state;
	struct wl_list link;
};

static struct wl_list ov_canvas_states = {&ov_canvas_states, &ov_canvas_states};

static struct {
	int mode;
	uint32_t button;
	Monitor *mon;
	Client *client;
	double start_x, start_y;
	double last_x, last_y;
	uint32_t last_configure_ms;
	bool moved;
} ov_drag = {0};

bool is_canvas_layout(Monitor *m) {
	if (!m || !m->pertag)
		return false;
	uint32_t tag = get_mon_curtag(m);
	const Layout *lt = m->pertag->ltidxs[tag];
	return lt && lt->name && strcmp(lt->name, "canvas") == 0;
}

static bool is_canvas_or_overview(Monitor *m) {
	return m && (m->isoverview || is_canvas_layout(m));
}

static void overview_canvas_configure_client_size(Client *c) {
	if (!c || !c->mon || c->mon->isoverview || !is_canvas_layout(c->mon) ||
		!c->ov_world_initialized || c->iskilling)
		return;

	int32_t min_size = 2 * (int32_t)c->bw + 24;
	int32_t ww = MANGO_MAX(min_size, (int32_t)roundf(c->ov_world_geom.width));
	int32_t wh = MANGO_MAX(min_size, (int32_t)roundf(c->ov_world_geom.height));
	uint32_t cw = (uint32_t)MANGO_MAX(1, ww - 2 * (int32_t)c->bw);
	uint32_t ch = (uint32_t)MANGO_MAX(1, wh - 2 * (int32_t)c->bw);
	client_set_size(c, cw, ch, false);
}

static int ov_zoom_timer_cb(void *data) {
	struct ov_canvas_state *st = data;
	Monitor *m = st->mon;
	if (!m || !is_canvas_or_overview(m)) {
		st->cam_zoom = st->target_cam_zoom;
		st->cam_x = st->target_cam_x;
		st->cam_y = st->target_cam_y;
		st->camera_animating = false;
		return 0;
	}

	float dz = st->target_cam_zoom - st->cam_zoom;
	float dx = st->target_cam_x - st->cam_x;
	float dy = st->target_cam_y - st->cam_y;

	if (fabsf(dz) < 0.001f && fabsf(dx) < 0.35f && fabsf(dy) < 0.35f) {
		st->cam_zoom = st->target_cam_zoom;
		st->cam_x = st->target_cam_x;
		st->cam_y = st->target_cam_y;
		st->camera_animating = false;
		overview_canvas_apply(m, false);
		return 0;
	}

	st->camera_animating = true;
	const float smooth = 0.22f;
	st->cam_zoom += dz * smooth;
	if (st->zoom_use_anchor) {
		st->cam_x = st->anchor_wx - st->anchor_rx / st->cam_zoom;
		st->cam_y = st->anchor_wy - st->anchor_ry / st->cam_zoom;
	} else {
		st->cam_x += dx * smooth;
		st->cam_y += dy * smooth;
	}

	overview_canvas_apply(m, false);
	if (st->zoom_timer)
		wl_event_source_timer_update(st->zoom_timer, 8);
	return 0;
}

static struct ov_canvas_state *ov_canvas_get(Monitor *m) {
	struct ov_canvas_state *st;
	wl_list_for_each(st, &ov_canvas_states, link) {
		if (st->mon == m)
			return st;
	}
	st = ecalloc(1, sizeof(*st));
	st->mon = m;
	st->cam_x = 0.0f;
	st->cam_y = 0.0f;
	st->cam_zoom = 1.0f;
	st->target_cam_x = 0.0f;
	st->target_cam_y = 0.0f;
	st->target_cam_zoom = 1.0f;
	st->zoom_use_anchor = false;
	st->camera_animating = false;
	st->custom_state = false;
	if (server.event_loop)
		st->zoom_timer =
			wl_event_loop_add_timer(server.event_loop, ov_zoom_timer_cb, st);
	wl_list_insert(&ov_canvas_states, &st->link);
	return st;
}

static void ov_canvas_stop_zoom_timer(struct ov_canvas_state *st) {
	if (!st)
		return;
	if (st->zoom_timer)
		wl_event_source_timer_update(st->zoom_timer, 0);
	st->target_cam_x = st->cam_x;
	st->target_cam_y = st->cam_y;
	st->target_cam_zoom = st->cam_zoom;
	st->zoom_use_anchor = false;
	st->camera_animating = false;
}

void overview_canvas_reset(Monitor *m) {
	if (!m)
		return;
	struct ov_canvas_state *st = ov_canvas_get(m);
	ov_canvas_stop_zoom_timer(st);
	st->cam_x = 0.0f;
	st->cam_y = 0.0f;
	st->cam_zoom = 1.0f;
	st->target_cam_x = 0.0f;
	st->target_cam_y = 0.0f;
	st->target_cam_zoom = 1.0f;
	st->custom_state = false;
	if (ov_drag.mon == m) {
		ov_drag.mode = OV_DRAG_NONE;
		ov_drag.client = NULL;
		ov_drag.moved = false;
	}
}

bool overview_canvas_has_custom_state(Monitor *m) {
	if (!m)
		return false;
	return ov_canvas_get(m)->custom_state;
}

void overview_canvas_capture_initial(Monitor *m) {
	if (!m)
		return;
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (!c || c->mon != m || !c->ov_card_tree || c->iskilling)
			continue;
		c->ov_world_geom = (struct wlr_fbox){
			.x = (float)c->geom.x,
			.y = (float)c->geom.y,
			.width = (float)c->geom.width,
			.height = (float)c->geom.height,
		};
		c->ov_world_initialized = true;
	}
}

/* Smoothly glides the camera in Canvas layout so Client `c` is centered on
 * Monitor `m` (like the Hyprland infinite canvas showcase). */
static void canvas_pan_camera_to_client(Monitor *m, Client *c) {
	if (!m || !c || !c->ov_world_initialized)
		return;

	struct ov_canvas_state *st = ov_canvas_get(m);
	float zoom = st->target_cam_zoom > 0.05f
					 ? st->target_cam_zoom
					 : (st->cam_zoom > 0.05f ? st->cam_zoom : 1.0f);

	float target_x = (c->ov_world_geom.x - (float)m->m.x) +
					 c->ov_world_geom.width * 0.5f -
					 ((float)m->m.width * 0.5f) / zoom;
	float target_y = (c->ov_world_geom.y - (float)m->m.y) +
					 c->ov_world_geom.height * 0.5f -
					 ((float)m->m.height * 0.5f) / zoom;

	st->custom_state = true;
	st->zoom_use_anchor = false;
	st->target_cam_zoom = zoom;
	st->target_cam_x = target_x;
	st->target_cam_y = target_y;

	if (!st->zoom_timer && server.event_loop)
		st->zoom_timer =
			wl_event_loop_add_timer(server.event_loop, ov_zoom_timer_cb, st);

	if (st->zoom_timer) {
		ov_zoom_timer_cb(st);
	} else {
		st->cam_x = target_x;
		st->cam_y = target_y;
		overview_canvas_apply(m, true);
	}
}

void canvas_focus_client(Monitor *m, Client *c, bool center_camera) {
	if (!m || !c)
		return;

	Client *prev_sel = m->sel;
	wlr_scene_node_raise_to_top(&c->scene->node);
	client_focus(c, 1);

	if (prev_sel && prev_sel != c && !prev_sel->iskilling)
		client_update_border_color(prev_sel);
	client_update_border_color(c);

	if (center_camera) {
		canvas_pan_camera_to_client(m, c);
		if (config.warpcursor) {
			wlr_cursor_warp_closest(server.cursor, NULL,
									m->m.x + m->m.width / 2.0,
									m->m.y + m->m.height / 2.0);
		}
	}
}

bool canvas_focus_dir(Monitor *m, int32_t dir) {
	if (!m || !is_canvas_layout(m))
		return false;

	overview_canvas_sync_clients(m);

	Client *tc = m->sel;
	if (!tc || tc->mon != m || tc->iskilling || tc->isminimized ||
		tc->isunglobal || !VISIBLEON(tc, m)) {
		Client *first = NULL;
		wl_list_for_each(first, &server.focus_stack, flink) {
			if (first->mon == m && !first->iskilling && !first->isminimized &&
				!first->isunglobal && VISIBLEON(first, m)) {
				canvas_focus_client(m, first, true);
				return true;
			}
		}
		return false;
	}

	float tc_cx = tc->ov_world_geom.x + tc->ov_world_geom.width * 0.5f;
	float tc_cy = tc->ov_world_geom.y + tc->ov_world_geom.height * 0.5f;

	Client *best = NULL;
	float best_score = FLT_MAX;

	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (!c || c == tc || c->mon != m || c->iskilling || c->isminimized ||
			c->isunglobal || !VISIBLEON(c, m) || !c->ov_world_initialized)
			continue;

		float c_cx = c->ov_world_geom.x + c->ov_world_geom.width * 0.5f;
		float c_cy = c->ov_world_geom.y + c->ov_world_geom.height * 0.5f;

		float dx = c_cx - tc_cx;
		float dy = c_cy - tc_cy;
		float primary = 0.0f;
		float secondary = 0.0f;

		switch (dir) {
		case LEFT:
			if (dx >= -1.0f)
				continue;
			primary = -dx;
			secondary = fabsf(dy);
			break;
		case RIGHT:
			if (dx <= 1.0f)
				continue;
			primary = dx;
			secondary = fabsf(dy);
			break;
		case UP:
			if (dy >= -1.0f)
				continue;
			primary = -dy;
			secondary = fabsf(dx);
			break;
		case DOWN:
			if (dy <= 1.0f)
				continue;
			primary = dy;
			secondary = fabsf(dx);
			break;
		default:
			continue;
		}

		float cone_penalty = (secondary > primary * 1.5f) ? 100000.0f : 0.0f;
		float score = cone_penalty + primary + secondary * 1.8f;

		if (score < best_score) {
			best_score = score;
			best = c;
		}
	}

	if (best) {
		canvas_focus_client(m, best, true);
		return true;
	}
	return false;
}

void overview_canvas_sync_clients(Monitor *m) {
	if (!m)
		return;
	struct ov_canvas_state *st = ov_canvas_get(m);
	float zoom = st->cam_zoom > 0.05f ? st->cam_zoom : 1.0f;
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (!c || c->mon != m || c->iskilling || c->isminimized ||
			c->isunglobal || !VISIBLEON(c, m))
			continue;
		if (!c->ov_card_tree)
			overview_backup_surface(c);
		if (!c->ov_world_initialized) {
			float w = c->geom.width > 100 ? (float)c->geom.width
										  : (float)m->w.width * 0.55f;
			float h = c->geom.height > 100 ? (float)c->geom.height
										   : (float)m->w.height * 0.6f;
			float center_wx =
				(float)m->m.x + st->cam_x +
				((float)m->m.width / zoom - w) * 0.5f;
			float center_wy =
				(float)m->m.y + st->cam_y +
				((float)m->m.height / zoom - h) * 0.5f;

			/* Place new windows to the right/below existing windows on the
			 * canvas so directional focus immediately has a spatial layout */
			for (int attempt = 0; attempt < 12; attempt++) {
				bool collision = false;
				Client *other;
				wl_list_for_each(other, &server.clients, link) {
					if (other == c || other->mon != m ||
						!other->ov_world_initialized || other->iskilling ||
						!VISIBLEON(other, m))
						continue;
					if (fabsf(other->ov_world_geom.x - center_wx) < w * 0.65f &&
						fabsf(other->ov_world_geom.y - center_wy) < h * 0.65f) {
						collision = true;
						break;
					}
				}
				if (!collision)
					break;
				center_wx += (attempt % 2 == 0) ? (w + 40.0f) : 0.0f;
				center_wy += (attempt % 2 == 1) ? (h + 40.0f) : 0.0f;
			}

			c->ov_world_geom = (struct wlr_fbox){
				.x = center_wx,
				.y = center_wy,
				.width = w,
				.height = h,
			};
			c->ov_world_initialized = true;
			st->custom_state = true;
			overview_canvas_configure_client_size(c);
		}
	}
}

void overview_canvas_apply(Monitor *m, bool animate) {
	if (!is_canvas_or_overview(m))
		return;

	struct ov_canvas_state *st = ov_canvas_get(m);
	float zoom = st->cam_zoom > 0.05f ? st->cam_zoom : 1.0f;
	Client *c;

	wl_list_for_each(c, &server.clients, link) {
		if (!c || c->mon != m || !c->ov_card_tree || c->iskilling ||
			!VISIBLEON(c, m))
			continue;
		if (!c->ov_world_initialized)
			continue;

		int32_t min_size = 2 * (int32_t)c->bw + 24;
		struct wlr_box geo = {
			.x = (int32_t)roundf((float)m->m.x +
								 (c->ov_world_geom.x - (float)m->m.x -
								  st->cam_x) *
									 zoom),
			.y = (int32_t)roundf((float)m->m.y +
								 (c->ov_world_geom.y - (float)m->m.y -
								  st->cam_y) *
									 zoom),
			.width = MANGO_MAX(min_size,
							   (int32_t)roundf(c->ov_world_geom.width * zoom)),
			.height = MANGO_MAX(
				min_size, (int32_t)roundf(c->ov_world_geom.height * zoom)),
		};

		if (animate && !st->camera_animating) {
			resize(c, geo,
				   (ResizeOpts){.interact = 1, .skip_ov_enter_anim = true});
		} else {
			c->geom = c->pending = c->current = c->animainit_geom =
				c->animation.initial = c->animation.current = geo;
			c->animation.running = false;
			c->animation.tagining = false;
			c->animation.tagouting = false;
			c->is_pending_open_animation = false;
			c->need_output_flush = false;
			c->dirty = false;
			wlr_scene_node_set_position(&c->scene->node, geo.x, geo.y);
			client_apply_clip(c, 1.0f);
		}
	}

	request_fresh_all_monitors();
}

/* Dedicated Infinite Canvas Layout Mode ("canvas") */
void canvas_layout(Monitor *m) {
	if (!m)
		return;
	overview_canvas_sync_clients(m);
	overview_canvas_apply(m, false);
}

bool canvas_predict(Monitor *m, Client *c, struct wlr_box *out_geo) {
	if (!m || !out_geo)
		return false;
	*out_geo = (struct wlr_box){
		.x = m->w.x + (int32_t)(m->w.width * 0.22f),
		.y = m->w.y + (int32_t)(m->w.height * 0.2f),
		.width = (int32_t)(m->w.width * 0.55f),
		.height = (int32_t)(m->w.height * 0.6f),
	};
	return true;
}

Client *overview_client_at(Monitor *m, double lx, double ly) {
	if (!is_canvas_or_overview(m))
		return NULL;

	Client *c;
	/* Walk focus_stack so the topmost focused/raised card wins when cards
	 * overlap on the canvas. */
	wl_list_for_each(c, &server.focus_stack, flink) {
		if (!c || c->mon != m || !c->ov_card_tree || c->iskilling ||
			c->isminimized || c->isunglobal)
			continue;
		if (!VISIBLEON(c, m))
			continue;
		struct wlr_box box = c->animation.current;
		if (box.width <= 0 || box.height <= 0)
			box = c->geom;
		if (wlr_box_contains_point(&box, lx, ly))
			return c;
	}
	return NULL;
}

void overview_canvas_zoom_at(Monitor *m, double lx, double ly, double delta) {
	if (!is_canvas_or_overview(m) || delta == 0.0)
		return;

	struct ov_canvas_state *st = ov_canvas_get(m);
	if (!st->custom_state) {
		overview_canvas_capture_initial(m);
		st->custom_state = true;
	}

	float cur_zoom = st->cam_zoom > 0.05f ? st->cam_zoom : 1.0f;
	float base_target =
		st->target_cam_zoom > 0.05f ? st->target_cam_zoom : cur_zoom;
	float factor = delta < 0.0 ? 1.15f : (1.0f / 1.15f);
	float new_zoom = fmaxf(0.15f, fminf(3.0f, base_target * factor));
	if (fabsf(new_zoom - base_target) < 0.0001f)
		return;

	float rel_x = (float)lx - (float)m->m.x;
	float rel_y = (float)ly - (float)m->m.y;

	/* World point currently under the cursor (relative to monitor origin) */
	float wx = st->cam_x + rel_x / cur_zoom;
	float wy = st->cam_y + rel_y / cur_zoom;

	st->anchor_wx = wx;
	st->anchor_wy = wy;
	st->anchor_rx = rel_x;
	st->anchor_ry = rel_y;
	st->zoom_use_anchor = true;

	st->target_cam_zoom = new_zoom;
	st->target_cam_x = wx - rel_x / new_zoom;
	st->target_cam_y = wy - rel_y / new_zoom;

	if (!st->zoom_timer && server.event_loop)
		st->zoom_timer =
			wl_event_loop_add_timer(server.event_loop, ov_zoom_timer_cb, st);

	if (st->zoom_timer) {
		ov_zoom_timer_cb(st);
	} else {
		st->cam_zoom = st->target_cam_zoom;
		st->cam_x = st->target_cam_x;
		st->cam_y = st->target_cam_y;
		overview_canvas_apply(m, false);
	}
}

bool overview_canvas_is_dragging(void) { return ov_drag.mode != OV_DRAG_NONE; }

bool overview_canvas_begin_move_resize(Client *c, uint32_t mode, double lx,
									   double ly) {
	if (!c || !c->mon || !is_canvas_layout(c->mon) || c->mon->isoverview)
		return false;

	Monitor *m = c->mon;
	overview_canvas_sync_clients(m);
	struct ov_canvas_state *st = ov_canvas_get(m);
	st->custom_state = true;

	ov_drag.mon = m;
	ov_drag.client = c;
	ov_drag.button = (mode == CurResize) ? BTN_RIGHT : BTN_LEFT;
	ov_drag.start_x = ov_drag.last_x = lx;
	ov_drag.start_y = ov_drag.last_y = ly;
	ov_drag.last_configure_ms = 0;
	ov_drag.moved = true;
	ov_drag.mode =
		(mode == CurResize) ? OV_DRAG_RESIZE_CLIENT : OV_DRAG_MOVE_CLIENT;

	wlr_scene_node_raise_to_top(&c->scene->node);
	client_focus(c, 1);

	wlr_cursor_set_xcursor(server.cursor, server.cursor_manager,
						   (mode == CurResize) ? "se-resize" : "grab");
	return true;
}

bool overview_canvas_button_press(Monitor *m, uint32_t button, double lx,
								  double ly) {
	if (!is_canvas_or_overview(m))
		return false;

	if (button != BTN_LEFT && button != BTN_RIGHT && button != BTN_MIDDLE)
		return false;

	Client *c = overview_client_at(m, lx, ly);

	ov_drag.mon = m;
	ov_drag.button = button;
	ov_drag.client = c;
	ov_drag.start_x = ov_drag.last_x = lx;
	ov_drag.start_y = ov_drag.last_y = ly;
	ov_drag.last_configure_ms = 0;
	ov_drag.moved = false;

	if (button == BTN_MIDDLE) {
		/* Strictly Middle Mouse Button pans the overview / canvas */
		ov_canvas_stop_zoom_timer(ov_canvas_get(m));
		ov_drag.mode = OV_DRAG_PAN_CANVAS;
		return true;
	}

	if (!m->isoverview)
		return false;

	if (button == BTN_LEFT && c) {
		ov_drag.mode = OV_DRAG_MOVE_CLIENT;
		wlr_scene_node_raise_to_top(&c->scene->node);
		client_focus(c, 1);
		return true;
	}

	if (button == BTN_RIGHT && c) {
		/* Overview does not resize cards; right-click closes on release */
		ov_drag.mode = OV_DRAG_CLICK_ONLY;
		return true;
	}

	ov_drag.mode = OV_DRAG_CLICK_ONLY;
	return true;
}

bool overview_canvas_motion(Monitor *m, double lx, double ly) {
	if (!is_canvas_or_overview(m))
		return false;

	struct ov_canvas_state *st = ov_canvas_get(m);

	if (ov_drag.mode == OV_DRAG_NONE) {
		if (!st->camera_animating && config.sloppyfocus) {
			Client *hc = overview_client_at(m, lx, ly);
			if (hc && m->sel != hc)
				client_focus(hc, 0);
		}
		return false;
	}

	if (ov_drag.mode == OV_DRAG_CLICK_ONLY) {
		double tdx = lx - ov_drag.start_x;
		double tdy = ly - ov_drag.start_y;
		if (tdx * tdx + tdy * tdy >= 25.0)
			ov_drag.moved = true;
		return true;
	}

	if (!ov_drag.moved) {
		double tdx = lx - ov_drag.start_x;
		double tdy = ly - ov_drag.start_y;
		if (tdx * tdx + tdy * tdy < 25.0)
			return true;

		if (!st->custom_state) {
			overview_canvas_capture_initial(m);
			st->custom_state = true;
		}
		ov_drag.moved = true;

		if (ov_drag.mode == OV_DRAG_RESIZE_CLIENT)
			wlr_cursor_set_xcursor(server.cursor, server.cursor_manager,
								   "se-resize");
		else
			wlr_cursor_set_xcursor(server.cursor, server.cursor_manager,
								   "grab");
	}

	double dx = lx - ov_drag.last_x;
	double dy = ly - ov_drag.last_y;
	ov_drag.last_x = lx;
	ov_drag.last_y = ly;

	float zoom = st->cam_zoom > 0.05f ? st->cam_zoom : 1.0f;

	if (ov_drag.mode == OV_DRAG_MOVE_CLIENT && ov_drag.client) {
		ov_drag.client->ov_world_geom.x += (float)(dx / zoom);
		ov_drag.client->ov_world_geom.y += (float)(dy / zoom);
		overview_canvas_apply(m, false);
	} else if (ov_drag.mode == OV_DRAG_RESIZE_CLIENT && ov_drag.client) {
		ov_drag.client->ov_world_geom.width = fmaxf(
			80.0f, ov_drag.client->ov_world_geom.width + (float)(dx / zoom));
		ov_drag.client->ov_world_geom.height = fmaxf(
			60.0f, ov_drag.client->ov_world_geom.height + (float)(dy / zoom));
		overview_canvas_apply(m, false);

		uint32_t now = get_now_in_ms();
		if (ov_drag.last_configure_ms == 0 ||
			now - ov_drag.last_configure_ms >= drag_refresh_interval_ms(m)) {
			overview_canvas_configure_client_size(ov_drag.client);
			ov_drag.last_configure_ms = now;
		}
	} else if (ov_drag.mode == OV_DRAG_PAN_CANVAS) {
		st->cam_x -= (float)(dx / zoom);
		st->cam_y -= (float)(dy / zoom);
		st->target_cam_x = st->cam_x;
		st->target_cam_y = st->cam_y;
		overview_canvas_apply(m, false);
	}

	return true;
}

bool overview_canvas_button_release(Monitor *m, uint32_t button, double lx,
									double ly) {
	if (ov_drag.mode == OV_DRAG_NONE)
		return false;

	int mode = ov_drag.mode;
	Client *c = ov_drag.client;
	bool moved = ov_drag.moved;
	Monitor *drag_mon = ov_drag.mon ? ov_drag.mon : m;

	ov_drag.mode = OV_DRAG_NONE;
	ov_drag.client = NULL;
	ov_drag.moved = false;

	if (!server.cursor_hidden)
		wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");

	if (mode == OV_DRAG_RESIZE_CLIENT && c) {
		overview_canvas_configure_client_size(c);
	}

	if (!moved && drag_mon) {
		if (drag_mon->isoverview && button == BTN_LEFT && c) {
			toggle_overview(&(Arg){.tc = c});
		} else if (drag_mon->isoverview && button == BTN_RIGHT && c) {
			pending_kill_client(c);
		} else if (button == BTN_MIDDLE && mode == OV_DRAG_PAN_CANVAS) {
			struct ov_canvas_state *st = ov_canvas_get(drag_mon);
			st->zoom_use_anchor = false;
			st->target_cam_x = 0.0f;
			st->target_cam_y = 0.0f;
			st->target_cam_zoom = 1.0f;
			if (st->zoom_timer) {
				ov_zoom_timer_cb(st);
			} else {
				st->cam_x = 0.0f;
				st->cam_y = 0.0f;
				st->cam_zoom = 1.0f;
				overview_canvas_apply(drag_mon, true);
			}
		}
	}

	return true;
}

// Returns 0 when the target window shares its tag with other windows.
uint32_t want_restore_fullscreen(Client *target_client) {
	Client *c = NULL;
	wl_list_for_each(c, &server.clients, link) {
		if (c && c != target_client && c->tags == target_client->tags &&
			c == server.selected_monitor->sel && c->mon &&
			c->mon->pertag->ltidxs[get_tags_first_tag_num(c->tags)]->id !=
				SCROLLER &&
			c->mon->pertag->ltidxs[get_tags_first_tag_num(c->tags)]->id !=
				VERTICAL_SCROLLER) {
			return 0;
		}
	}

	return 1;
}

// Recomputes layout after surface commit (scene_surface commit resets
// dest/source and needs to be reapplied).
void handle_overview_card_surface_commit(struct wl_listener *listener,
										 void *data) {
	struct ov_card_surface *entry = wl_container_of(listener, entry, commit);
	if (entry->c && entry->c->ov_card_tree)
		overview_layout_card(entry->c);
}

// Removes and frees the node when the surface is destroyed.
void handle_overview_card_surface_destroy(struct wl_listener *listener,
										  void *data) {
	struct ov_card_surface *entry = wl_container_of(listener, entry, destroy);
	wl_list_remove(&entry->link);
	wl_list_remove(&entry->commit.link);
	wl_list_remove(&entry->destroy.link);
	free(entry);
}

// Creates a card scene_surface node for every surface (including subsurfaces).
void overview_card_surface_add(struct wlr_surface *surface, int sx, int sy,
							   void *data) {
	Client *c = data;
	if (!c->ov_card_tree)
		return;

	struct ov_card_surface *entry = ecalloc(1, sizeof(*entry));
	entry->c = c;
	entry->surface = surface;
	entry->sx = sx;
	entry->sy = sy;
	entry->is_root = (surface == client_surface(c));

	entry->scene_surface = wlr_scene_surface_create(c->ov_card_tree, surface);
	if (!entry->scene_surface) {
		free(entry);
		return;
	}
	entry->buffer = entry->scene_surface->buffer;
	wlr_scene_buffer_set_filter_mode(entry->buffer, WLR_SCALE_FILTER_BILINEAR);

	entry->commit.notify = handle_overview_card_surface_commit;
	wl_signal_add(&surface->events.commit, &entry->commit);
	entry->destroy.notify = handle_overview_card_surface_destroy;
	wl_signal_add(&surface->events.destroy, &entry->destroy);

	wl_list_insert(&c->ov_card_surfaces, &entry->link);
}

// Updates card position and scale from the current geometry; content origin
// uses client_get_clip geometry offset.
void overview_layout_card(Client *c) {
	if (!c->ov_card_tree)
		return;

	struct wlr_box geo = c->animation.current;
	if (geo.width <= 0 || geo.height <= 0)
		client_get_geometry(c, &geo);
	int32_t bw = (int32_t)c->bw;
	int32_t w = geo.width - 2 * bw;
	int32_t h = geo.height - 2 * bw;
	if (w <= 0 || h <= 0)
		return;

	wlr_scene_node_set_position(&c->ov_card_tree->node, bw, bw);

	// Content origin (geometry offset) and card content size.
	struct wlr_box clip;
	client_get_clip(c, &clip);

	float content_w, content_h;
#ifdef XWAYLAND
	struct wlr_surface *s = client_surface(c);
	if (client_is_x11(c)) {
		content_w = s->current.width;
		content_h = s->current.height;
	} else
#endif
	{
		content_w = c->surface.xdg->geometry.width;
		content_h = c->surface.xdg->geometry.height;
	}
	if (content_w <= 0 || content_h <= 0)
		return;

	float scale_x = (float)w / content_w;
	float scale_y = (float)h / content_h;

	int32_t vx = 0, vy = 0, vw = w, vh = h;
	if (c->mon) {
		struct wlr_box content_box = {
			.x = geo.x + bw,
			.y = geo.y + bw,
			.width = w,
			.height = h,
		};
		struct wlr_box vis;
		if (wlr_box_intersection(&vis, &content_box, &c->mon->m)) {
			vx = vis.x - content_box.x;
			vy = vis.y - content_box.y;
			vw = vis.width;
			vh = vis.height;
		} else {
			vw = 0;
			vh = 0;
		}
	}

	struct ov_card_surface *entry;
	wl_list_for_each(entry, &c->ov_card_surfaces, link) {
		struct wlr_surface *es = entry->surface;
		float lw = es->current.width;
		float lh = es->current.height;

		if (entry->is_root) {
			struct wlr_fbox bsrc = {0};
			wlr_surface_get_buffer_source_box(es, &bsrc);
			float ratio_x = es->current.width > 0
								? (float)bsrc.width / es->current.width
								: 1.0f;
			float ratio_y = es->current.height > 0
								? (float)bsrc.height / es->current.height
								: 1.0f;
			if (vw <= 0 || vh <= 0) {
				wlr_scene_node_set_position(&entry->buffer->node, 0, 0);
				wlr_scene_buffer_set_dest_size(entry->buffer, 0, 0);
				continue;
			}
			wlr_scene_node_set_position(&entry->buffer->node, vx, vy);
			wlr_scene_buffer_set_dest_size(entry->buffer, vw, vh);
			struct wlr_fbox src = {
				.x = bsrc.x + (clip.x + (float)vx / scale_x) * ratio_x,
				.y = bsrc.y + (clip.y + (float)vy / scale_y) * ratio_y,
				.width = ((float)vw / scale_x) * ratio_x,
				.height = ((float)vh / scale_y) * ratio_y,
			};
			wlr_scene_buffer_set_source_box(entry->buffer, &src);
		} else {
			int px = (int)((entry->sx - clip.x) * scale_x);
			int py = (int)((entry->sy - clip.y) * scale_y);
			int dw = (int)(lw * scale_x);
			int dh = (int)(lh * scale_y);

			int cx0 = MANGO_MAX(px, vx);
			int cy0 = MANGO_MAX(py, vy);
			int cx1 = MANGO_MIN(px + dw, vx + vw);
			int cy1 = MANGO_MIN(py + dh, vy + vh);
			int cw = cx1 - cx0;
			int ch = cy1 - cy0;
			if (cw <= 0 || ch <= 0) {
				wlr_scene_node_set_position(&entry->buffer->node, 0, 0);
				wlr_scene_buffer_set_dest_size(entry->buffer, 0, 0);
				continue;
			}

			struct wlr_fbox bsrc = {0};
			wlr_surface_get_buffer_source_box(es, &bsrc);
			float ratio_x = es->current.width > 0
								? (float)bsrc.width / es->current.width
								: 1.0f;
			float ratio_y = es->current.height > 0
								? (float)bsrc.height / es->current.height
								: 1.0f;
			float ox = (float)(cx0 - px) / scale_x;
			float oy = (float)(cy0 - py) / scale_y;
			wlr_scene_node_set_position(&entry->buffer->node, cx0, cy0);
			wlr_scene_buffer_set_dest_size(entry->buffer, cw, ch);
			struct wlr_fbox src = {
				.x = bsrc.x + ox * ratio_x,
				.y = bsrc.y + oy * ratio_y,
				.width = ((float)cw / scale_x) * ratio_x,
				.height = ((float)ch / scale_y) * ratio_y,
			};
			wlr_scene_buffer_set_source_box(entry->buffer, &src);
		}
	}

	overview_update_jump_label(c);
}

void overview_update_jump_label(Client *c) {
	if (!c || !c->mon || !c->ov_card_tree || !c->mon->isoverview ||
		!c->mon->is_jump_mode || !c->jump_char)
		return;

	struct wlr_scene_node *label = &c->jump_label_node->scene->node;
	int32_t lw = c->jump_label_node->logical_width;
	int32_t lh = c->jump_label_node->logical_height;
	if (lw <= 0 || lh <= 0) {
		if (label->enabled)
			wlr_scene_node_set_enabled(label, false);
		return;
	}

	struct wlr_box cur = c->animation.current;
	int32_t lx = cur.x + (cur.width - lw) / 2;
	int32_t ly = cur.y + (cur.height - lh) / 2;
	if (lx < c->mon->m.x || ly < c->mon->m.y ||
		lx + lw > c->mon->m.x + c->mon->m.width ||
		ly + lh > c->mon->m.y + c->mon->m.height) {
		if (label->enabled)
			wlr_scene_node_set_enabled(label, false);
		return;
	}

	wlr_scene_node_set_position(label, (cur.width - lw) / 2,
								(cur.height - lh) / 2);
	if (!label->enabled) {
		wlr_scene_node_set_enabled(label, true);
		wlr_scene_node_raise_to_top(label);
	}
}

// Destroys the card tree and frees all surface nodes.
void overview_destroy_card(Client *c) {
	if (ov_drag.client == c) {
		ov_drag.mode = OV_DRAG_NONE;
		ov_drag.client = NULL;
		ov_drag.moved = false;
	}

	if (!c->ov_card_tree)
		return;

	struct ov_card_surface *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &c->ov_card_surfaces, link) {
		wl_list_remove(&entry->commit.link);
		wl_list_remove(&entry->destroy.link);
		wl_list_remove(&entry->link);
		free(entry);
	}

	wlr_scene_node_destroy(&c->ov_card_tree->node);
	c->ov_card_tree = NULL;
}

// Applies rounded corners to all buffer nodes of the card.
void overview_card_set_corner_radii(Client *c, struct fx_corner_radii corners) {
	struct ov_card_surface *entry;
	wl_list_for_each(entry, &c->ov_card_surfaces, link)
		wlr_scene_buffer_set_corner_radii(entry->buffer, corners);
}

// Entering overview: saves and disables the real scene_surface tree and builds
// an independent card tree to display content.
void overview_backup_surface(Client *c) {
	if (c->ov_card_tree)
		return;
	if (!client_surface(c) || !client_surface(c)->mapped)
		return;

	// Disables the real surface tree.
	c->overview_scene_surface = c->scene_surface;

	// In overview every tag window must show its card and must not be disabled
	// by the subtree hiding logic.
	c->is_clip_to_hide = false;
	client_update_visibility(c);

	c->ov_card_tree = wlr_scene_tree_create(c->scene);
	if (!c->ov_card_tree)
		return;

	// Walks the surface tree and creates a card node per surface.
	wlr_surface_for_each_surface(client_surface(c), overview_card_surface_add,
								 c);

	// The card tree is created at the scene top; enabled jump labels are raised
	// above the cards.
	if (c->jump_label_node->scene->node.enabled)
		wlr_scene_node_raise_to_top(&c->jump_label_node->scene->node);

	overview_layout_card(c);

	// Feeds one frame to start the render loop (later driven by scene_surface
	// frame-done).
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	client_send_frame_done(c, &now);
}

// Saves the window old state when switching from the normal view to overview.
void overview_backup(Client *c) {
	if (c->mon)
		overview_canvas_reset(c->mon);
	c->ov_world_initialized = false;
	c->overview_isfloatingbak = c->isfloating;
	c->overview_isfullscreenbak = c->isfullscreen;
	c->overview_ismaximizescreenbak = c->ismaximizescreen;
	c->animation.tagining = false;
	c->animation.tagouted = false;
	c->animation.tagouting = false;
	c->overview_backup_geom = c->geom;
	c->overview_backup_bw = c->bw;
	if (c->isfloating) {
		c->isfloating = 0;
	}

	overview_backup_surface(c);

	if (c->isfullscreen || c->ismaximizescreen) {
		client_pending_fullscreen_state(
			c, 0); // Clears the window fullscreen flag.
		client_pending_maximized_state(c, 0);
	}
	c->bw = c->no_border ? 0 : (config.borderpx + config.outer_borderpx);

	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
							WLR_EDGE_RIGHT);
	client_sync_layer(c);
}

// Restores window state when switching back from overview to the normal view.
void overview_restore(Client *c, const Arg *arg) {
	if (!c->ov_card_tree && !c->overview_scene_surface)
		return;

	if (c->mon)
		overview_canvas_reset(c->mon);
	c->ov_world_initialized = false;

	struct wlr_box ov_card_box = c->animation.current;
	if (ov_card_box.width <= 0 || ov_card_box.height <= 0)
		ov_card_box = c->geom;

	c->isfloating = c->overview_isfloatingbak;
	c->isfullscreen = c->overview_isfullscreenbak;
	c->ismaximizescreen = c->overview_ismaximizescreenbak;
	c->overview_isfloatingbak = 0;
	c->overview_isfullscreenbak = 0;
	c->overview_ismaximizescreenbak = 0;
	c->bw = c->overview_backup_bw;
	c->animation.tagining = false;
	c->is_restoring_from_ov = (arg->ui & c->tags & TAGMASK) == 0 ? true : false;

	// Destroys the card tree and restores the real scene_surface tree.
	overview_destroy_card(c);
	if (c->overview_scene_surface) {
		c->scene_surface = c->overview_scene_surface;
		c->overview_scene_surface = NULL;
		client_update_visibility(c);
	}

	c->animation.current = ov_card_box;
	c->animainit_geom = ov_card_box;
	c->current = ov_card_box;

	if (c->isfloating) {
		resize(c, c->overview_backup_geom, (ResizeOpts){.interact = 0});
	} else if (c->isfullscreen || c->ismaximizescreen) {
		if (want_restore_fullscreen(c) && c->ismaximizescreen) {
			client_set_maximize_screen(c, 1, false);
		} else if (want_restore_fullscreen(c) && c->isfullscreen) {
			client_apply_fullscreen(c, 1, false);
		} else {
			client_pending_fullscreen_state(c, 0);
			client_pending_maximized_state(c, 0);
			client_apply_fullscreen(c, false, false);
		}
	} else {
		if (c->is_restoring_from_ov) {
			c->is_restoring_from_ov = false;
			resize(c, c->overview_backup_geom, (ResizeOpts){.interact = 0});
		}
	}

	if (c->bw == 0 && !c->isfullscreen) { // Windows created while in overview
										  // mode have no bw record.
		c->bw = c->no_border ? 0 : (config.borderpx + config.outer_borderpx);
	}

	if (c->isfloating && !c->force_tiled_state) {
		client_set_tiled(c, WLR_EDGE_NONE);
	}

	client_sync_layer(c);
}
