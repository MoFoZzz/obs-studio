/******************************************************************************
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "util/threading.h"
#include "graphics/math-defs.h"
#include "obs-scene.h"

static const char *obs_scene_signals[] = {
	"void item_add(ptr scene, ptr item)",
	"void item_remove(ptr scene, ptr item)",
	NULL
};

static inline void signal_item_remove(struct obs_scene_item *item)
{
	struct calldata params = {0};
	calldata_setptr(&params, "scene", item->parent);
	calldata_setptr(&params, "item", item);

	signal_handler_signal(item->parent->source->context.signals,
			"item_remove", &params);
	calldata_free(&params);
}

static const char *scene_getname(const char *locale)
{
	/* TODO: locale */
	UNUSED_PARAMETER(locale);
	return "Scene";
}

static void *scene_create(obs_data_t settings, struct obs_source *source)
{
	pthread_mutexattr_t attr;
	struct obs_scene *scene = bmalloc(sizeof(struct obs_scene));
	scene->source     = source;
	scene->first_item = NULL;

	signal_handler_add_array(obs_source_signalhandler(source),
			obs_scene_signals);

	if (pthread_mutexattr_init(&attr) != 0)
		goto fail;
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0)
		goto fail;
	if (pthread_mutex_init(&scene->mutex, &attr) != 0) {
		blog(LOG_ERROR, "scene_create: Couldn't initialize mutex");
		goto fail;
	}

	UNUSED_PARAMETER(settings);
	return scene;

fail:
	pthread_mutexattr_destroy(&attr);
	bfree(scene);
	return NULL;
}

static void remove_all_items(struct obs_scene *scene)
{
	struct obs_scene_item *item;

	pthread_mutex_lock(&scene->mutex);

	item = scene->first_item;

	while (item) {
		struct obs_scene_item *del_item = item;
		item = item->next;

		obs_sceneitem_remove(del_item);
	}

	pthread_mutex_unlock(&scene->mutex);
}

static void scene_destroy(void *data)
{
	struct obs_scene *scene = data;

	remove_all_items(scene);
	pthread_mutex_destroy(&scene->mutex);
	bfree(scene);
}

static void scene_enum_sources(void *data,
		obs_source_enum_proc_t enum_callback,
		void *param)
{
	struct obs_scene *scene = data;
	struct obs_scene_item *item;

	pthread_mutex_lock(&scene->mutex);

	item = scene->first_item;
	while (item) {
		struct obs_scene_item *next = item->next;

		obs_sceneitem_addref(item);
		enum_callback(scene->source, item->source, param);
		obs_sceneitem_release(item);

		item = next;
	}

	pthread_mutex_unlock(&scene->mutex);
}

static inline void detach_sceneitem(struct obs_scene_item *item)
{
	if (item->prev)
		item->prev->next = item->next;
	else
		item->parent->first_item = item->next;

	if (item->next)
		item->next->prev = item->prev;

	item->parent = NULL;
}

static inline void attach_sceneitem(struct obs_scene_item *item,
		struct obs_scene_item *prev)
{
	item->prev = prev;

	if (prev) {
		item->next = prev->next;
		if (prev->next)
			prev->next->prev = item;
		prev->next = item;
	} else {
		assert(item->parent != NULL);

		item->next = item->parent->first_item;
		item->parent->first_item = item;
	}
}

static void scene_video_render(void *data, effect_t effect)
{
	struct obs_scene *scene = data;
	struct obs_scene_item *item;

	pthread_mutex_lock(&scene->mutex);

	item = scene->first_item;

	while (item) {
		if (obs_source_removed(item->source)) {
			struct obs_scene_item *del_item = item;
			item = item->next;

			obs_sceneitem_remove(del_item);
			continue;
		}

		gs_matrix_push();
		gs_matrix_translate3f(item->origin.x, item->origin.y, 0.0f);
		gs_matrix_scale3f(item->scale.x, item->scale.y, 1.0f);
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(-item->rot));
		gs_matrix_translate3f(-item->pos.x, -item->pos.y, 0.0f);

		obs_source_video_render(item->source);

		gs_matrix_pop();

		item = item->next;
	}

	pthread_mutex_unlock(&scene->mutex);

	UNUSED_PARAMETER(effect);
}

static void scene_load_item(struct obs_scene *scene, obs_data_t item_data)
{
	const char            *name = obs_data_getstring(item_data, "name");
	obs_source_t          source = obs_get_source_by_name(name);
	struct obs_scene_item *item;

	if (!source) {
		blog(LOG_WARNING, "[scene_load_item] Source %s not found!",
				name);
		return;
	}

	item = obs_scene_add(scene, source);

	item->rot     = (float)obs_data_getdouble(item_data, "rot");
	item->visible = obs_data_getbool(item_data, "visible");
	obs_data_get_vec2(item_data, "origin", &item->origin);
	obs_data_get_vec2(item_data, "pos",    &item->pos);
	obs_data_get_vec2(item_data, "scale",  &item->scale);
	obs_source_release(source);
}

static void scene_load(void *scene, obs_data_t settings)
{
	obs_data_array_t items = obs_data_getarray(settings, "items");
	size_t           count, i;

	remove_all_items(scene);

	if (!items) return;

	count = obs_data_array_count(items);

	for (i = 0; i < count; i++) {
		obs_data_t item_data = obs_data_array_item(items, i);
		scene_load_item(scene, item_data);
		obs_data_release(item_data);
	}

	obs_data_array_release(items);
}

static void scene_save_item(obs_data_array_t array, struct obs_scene_item *item)
{
	obs_data_t item_data = obs_data_create();
	const char *name     = obs_source_getname(item->source);

	obs_data_setstring(item_data, "name",    name);
	obs_data_setbool  (item_data, "visible", item->visible);
	obs_data_setdouble(item_data, "rot",     item->rot);
	obs_data_set_vec2 (item_data, "origin",  &item->origin);
	obs_data_set_vec2 (item_data, "pos",     &item->pos);
	obs_data_set_vec2 (item_data, "scale",   &item->scale);

	obs_data_array_push_back(array, item_data);
	obs_data_release(item_data);
}

static void scene_save(void *data, obs_data_t settings)
{
	struct obs_scene      *scene = data;
	obs_data_array_t      array  = obs_data_array_create();
	struct obs_scene_item *item;

	pthread_mutex_lock(&scene->mutex);

	item = scene->first_item;
	while (item) {
		scene_save_item(array, item);
		item = item->next;
	}

	pthread_mutex_unlock(&scene->mutex);

	obs_data_setarray(settings, "items", array);
	obs_data_array_release(array);
}

static uint32_t scene_getwidth(void *data)
{
	UNUSED_PARAMETER(data);
	return obs->video.base_width;
}

static uint32_t scene_getheight(void *data)
{
	UNUSED_PARAMETER(data);
	return obs->video.base_height;
}

const struct obs_source_info scene_info =
{
	.id           = "scene",
	.type         = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.getname      = scene_getname,
	.create       = scene_create,
	.destroy      = scene_destroy,
	.video_render = scene_video_render,
	.getwidth     = scene_getwidth,
	.getheight    = scene_getheight,
	.load         = scene_load,
	.save         = scene_save,
	.enum_sources = scene_enum_sources
};

obs_scene_t obs_scene_create(const char *name)
{
	struct obs_source *source =
		obs_source_create(OBS_SOURCE_TYPE_INPUT, "scene", name, NULL);
	return source->context.data;
}

void obs_scene_addref(obs_scene_t scene)
{
	if (scene)
		obs_source_addref(scene->source);
}

void obs_scene_release(obs_scene_t scene)
{
	if (scene)
		obs_source_release(scene->source);
}

obs_source_t obs_scene_getsource(obs_scene_t scene)
{
	return scene ? scene->source : NULL;
}

obs_scene_t obs_scene_fromsource(obs_source_t source)
{
	if (!source || source->info.id != scene_info.id)
		return NULL;

	return source->context.data;
}

obs_sceneitem_t obs_scene_findsource(obs_scene_t scene, const char *name)
{
	struct obs_scene_item *item;

	if (!scene)
		return NULL;

	pthread_mutex_lock(&scene->mutex);

	item = scene->first_item;
	while (item) {
		if (strcmp(item->source->context.name, name) == 0)
			break;

		item = item->next;
	}

	pthread_mutex_unlock(&scene->mutex);

	return item;
}

void obs_scene_enum_items(obs_scene_t scene,
		bool (*callback)(obs_scene_t, obs_sceneitem_t, void*),
		void *param)
{
	struct obs_scene_item *item;

	if (!scene || !callback)
		return;

	pthread_mutex_lock(&scene->mutex);

	item = scene->first_item;
	while (item) {
		struct obs_scene_item *next = item->next;

		obs_sceneitem_addref(item);

		if (!callback(scene, item, param)) {
			obs_sceneitem_release(item);
			break;
		}

		obs_sceneitem_release(item);

		item = next;
	}

	pthread_mutex_unlock(&scene->mutex);
}

obs_sceneitem_t obs_scene_add(obs_scene_t scene, obs_source_t source)
{
	struct obs_scene_item *last;
	struct obs_scene_item *item;
	struct calldata params = {0};

	if (!scene)
		return NULL;

	if (!source) {
		blog(LOG_ERROR, "Tried to add a NULL source to a scene");
		return NULL;
	}

	item = bzalloc(sizeof(struct obs_scene_item));
	item->source  = source;
	item->visible = true;
	item->parent  = scene;
	item->ref     = 1;
	vec2_set(&item->scale, 1.0f, 1.0f);

	obs_source_addref(source);
	obs_source_add_child(scene->source, source);

	pthread_mutex_lock(&scene->mutex);

	last = scene->first_item;
	if (!last) {
		scene->first_item = item;
	} else {
		while (last->next)
			last = last->next;

		last->next = item;
		item->prev = last;
	}

	pthread_mutex_unlock(&scene->mutex);

	calldata_setptr(&params, "scene", scene);
	calldata_setptr(&params, "item", item);
	signal_handler_signal(scene->source->context.signals, "item_add",
			&params);
	calldata_free(&params);

	return item;
}

static void obs_sceneitem_destroy(obs_sceneitem_t item)
{
	if (item) {
		if (item->source)
			obs_source_release(item->source);
		bfree(item);
	}
}

void obs_sceneitem_addref(obs_sceneitem_t item)
{
	if (item)
		os_atomic_inc_long(&item->ref);
}

void obs_sceneitem_release(obs_sceneitem_t item)
{
	if (!item)
		return;

	if (os_atomic_dec_long(&item->ref) == 0)
		obs_sceneitem_destroy(item);
}

void obs_sceneitem_remove(obs_sceneitem_t item)
{
	obs_scene_t scene;

	if (!item)
		return;

	scene = item->parent;

	if (scene)
		pthread_mutex_lock(&scene->mutex);

	if (item->removed) {
		if (scene)
			pthread_mutex_unlock(&scene->mutex);
		return;
	}

	item->removed = true;

	assert(scene != NULL);
	assert(scene->source != NULL);
	obs_source_remove_child(scene->source, item->source);

	signal_item_remove(item);
	detach_sceneitem(item);

	pthread_mutex_unlock(&scene->mutex);

	obs_sceneitem_release(item);
}

obs_scene_t obs_sceneitem_getscene(obs_sceneitem_t item)
{
	return item ? item->parent : NULL;
}

obs_source_t obs_sceneitem_getsource(obs_sceneitem_t item)
{
	return item ? item->source : NULL;
}

void obs_sceneitem_setpos(obs_sceneitem_t item, const struct vec2 *pos)
{
	if (item)
		vec2_copy(&item->pos, pos);
}

void obs_sceneitem_setrot(obs_sceneitem_t item, float rot)
{
	if (item)
		item->rot = rot;
}

void obs_sceneitem_setorigin(obs_sceneitem_t item, const struct vec2 *origin)
{
	if (item)
		vec2_copy(&item->origin, origin);
}

void obs_sceneitem_setscale(obs_sceneitem_t item, const struct vec2 *scale)
{
	if (item)
		vec2_copy(&item->scale, scale);
}

void obs_sceneitem_setorder(obs_sceneitem_t item, enum order_movement movement)
{
	if (!item) return;

	struct obs_scene *scene = item->parent;

	obs_scene_addref(scene);
	pthread_mutex_lock(&scene->mutex);

	detach_sceneitem(item);

	if (movement == ORDER_MOVE_UP) {
		attach_sceneitem(item, item->prev);

	} else if (movement == ORDER_MOVE_DOWN) {
		attach_sceneitem(item, item->next);

	} else if (movement == ORDER_MOVE_TOP) {
		struct obs_scene_item *last = item->next;
		if (!last) {
			last = item->prev;
		} else {
			while (last->next)
				last = last->next;
		}

		attach_sceneitem(item, last);

	} else if (movement == ORDER_MOVE_BOTTOM) {
		attach_sceneitem(item, NULL);
	}

	pthread_mutex_unlock(&scene->mutex);
	obs_scene_release(scene);
}

void obs_sceneitem_getpos(obs_sceneitem_t item, struct vec2 *pos)
{
	if (item)
		vec2_copy(pos, &item->pos);
}

float obs_sceneitem_getrot(obs_sceneitem_t item)
{
	return item ? item->rot : 0.0f;
}

void obs_sceneitem_getorigin(obs_sceneitem_t item, struct vec2 *origin)
{
	if (item)
		vec2_copy(origin, &item->origin);
}

void obs_sceneitem_getscale(obs_sceneitem_t item, struct vec2 *scale)
{
	if (item)
		vec2_copy(scale, &item->scale);
}
