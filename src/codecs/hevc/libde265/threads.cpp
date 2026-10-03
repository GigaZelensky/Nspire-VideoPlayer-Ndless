/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

// Ndless port: no thread creation or blocking waits are available.
#include "threads.h"
#include <assert.h>

void de265_cond_wait(de265_cond*, de265_mutex*) { assert(!"unexpected threaded wait"); }
de265_progress_lock::de265_progress_lock() : mProgress(0) {}
de265_progress_lock::~de265_progress_lock() {}
void de265_progress_lock::wait_for_progress(int progress) { assert(mProgress >= progress); }
void de265_progress_lock::set_progress(int progress) { if (progress > mProgress) mProgress=progress; }
void de265_progress_lock::increase_progress(int progress) { mProgress += progress; }
int de265_progress_lock::get_progress() const { return mProgress; }
de265_error start_thread_pool(thread_pool*, int) { return DE265_ERROR_CANNOT_START_THREADPOOL; }
void stop_thread_pool(thread_pool*) {}
void add_task(thread_pool*, thread_task*) { assert(!"unexpected threaded task"); }
