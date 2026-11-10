#ifndef JER_CARGLOW_H
#define JER_CARGLOW_H

/* A slightly scaled low-LOD body copy, drawn semi-transparent with the
 * stock explosion material (smoke_texture), tinted, and pulsed so the
 * shell looks like it is looping. Per-instance verts (never the shared
 * template vlist). Self-contained: easy to lift out of this module later.
 *
 * Register jer_carglow_on_draw_world on JER_EVENT_DRAW_WORLD. One car at
 * a time: jer_carglow_set replaces the previous target.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* `car` is a CAR_DATA*. r,g,b are 0..255. */
void jer_carglow_set(void* car, int r, int g, int b);
void jer_carglow_clear(void);

/* Hook: JER_EVENT_DRAW_WORLD. Returns JER_RESULT_CONTINUE. */
int  jer_carglow_on_draw_world(void* userdata, void* args);

#ifdef __cplusplus
}
#endif

#endif /* JER_CARGLOW_H */
