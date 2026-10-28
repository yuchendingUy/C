/*
 * 机械动力：入口
 */
#ifndef MCC_CREATE_CREATE_H
#define MCC_CREATE_CREATE_H

namespace mcc::create {

/* 方块注册表载入之后调用：载入应力表、注册全部机械动力方块行为。data_dir 为 data 目录 */
bool create_init(const char *data_dir);

} // namespace mcc::create

#endif
