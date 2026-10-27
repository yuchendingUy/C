#ifndef MCC_PHYS_BOXBOX_H
#define MCC_PHYS_BOXBOX_H

#include <box3d/collision.h>

/* 长方体对长方体接触：A 在原点轴对齐（半尺寸 ha），B 经 xf（B → A）放置（半尺寸 hb）。
 * out->points 需至少 4 个容量。返回点数 */
int phys_collide_boxes(b3Vec3 ha, b3Vec3 hb, b3Transform xf, float speculative, b3LocalManifold *out);

#endif
