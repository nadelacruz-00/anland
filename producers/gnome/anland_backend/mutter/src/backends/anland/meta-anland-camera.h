/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

#include <glib.h>

typedef struct anland_camera MetaAnlandCamera;

MetaAnlandCamera * meta_anland_camera_new (void);
void meta_anland_camera_free (MetaAnlandCamera *camera);

void meta_anland_camera_set_resources (MetaAnlandCamera *camera,
                                       int               control_fd,
                                       const int        *stream_fds,
                                       int               n_stream_fds);
void meta_anland_camera_clear (MetaAnlandCamera *camera);
gboolean meta_anland_camera_has_resources (MetaAnlandCamera *camera);
