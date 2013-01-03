/** @file
  * @copyright Copyright (c) 2012 PROFACTOR GmbH. All rights reserved. 
  *
  * Redistribution and use in source and binary forms, with or without
  * modification, are permitted provided that the following conditions are
  * met:
  *
  *     * Redistributions of source code must retain the above copyright
  * notice, this list of conditions and the following disclaimer.
  *     * Redistributions in binary form must reproduce the above
  * copyright notice, this list of conditions and the following disclaimer
  * in the documentation and/or other materials provided with the
  * distribution.
  *     * Neither the name of Google Inc. nor the names of its
  * contributors may be used to endorse or promote products derived from
  * this software without specific prior written permission.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  * @authors christoph.kopf@profactor.at
  *          florian.eckerstorfer@profactor.at
  */
  
#pragma once

#define STATUS_MSG_DURATION 2000

#include "reme_resource_manager.h"
#include "settings.h"
#include "strings.h"

#include <QDebug>
#include <QCoreApplication>
#include <QSettings>
#include <QImage>
#include <QtConcurrentRun>

#include <reconstructmesdk/reme.h>

#include <sstream>
#include <iostream>

namespace ReconstructMeGUI {

  void reme_log(reme_log_severity_t sev, const char *message, void *user_data)  {
    reme_resource_manager *i = static_cast<reme_resource_manager*>(user_data);
    i->new_log_message(sev, QString(message));
  }

  reme_resource_manager::reme_resource_manager() {
    _c = 0;
    _initializing = false;
    connect(&_fw, SIGNAL(finished()), this, SLOT(finished_initialize()));
  }

  reme_resource_manager::~reme_resource_manager() {
    if (_c != 0)
      reme_context_destroy(&_c);
  }

  void reme_resource_manager::new_log_message(reme_log_severity_t sev, const QString &log) {
    emit log_message(sev, log);
  }

  void reme_resource_manager::_initialize() {
    bool success = false;

    _has_sensor = false;
    _has_compiled_context = false;
    _has_volume = false;

    emit initializing_sdk();

    if (_c != 0)
      reme_context_destroy(&_c);

    reme_context_create(&_c);
    reme_context_set_log_callback(_c, reme_log, this);
    
    emit initializing(LICENSE);
    success = apply_license();
    emit initialized(LICENSE, success);
    
    emit initializing(OPENCL);
    success = compile_context();
    emit initialized(OPENCL, success);
    
    emit initializing(SENSOR);
    success = open_sensor();
    emit initialized(SENSOR, success);
    
    success = _has_compiled_context && _has_sensor && _has_volume;
    emit sdk_initialized(success);
  }

  void _init(reme_resource_manager *initializer) {
    return initializer->_initialize();
  }

  void reme_resource_manager::initialize() {
    if (_initializing) return;

    _initializing = true;
    _future = QtConcurrent::run(_init, this);
    _fw.setFuture(_future);
  }

  void reme_resource_manager::finished_initialize() {
    _initializing = false;
  }


  bool reme_resource_manager::open_sensor() {
    bool success = true;
    
    // can not initialize sensor is no compiled context is available
    if (!_has_compiled_context) success = false;

    // create and open a sensor from settings
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, profactor_tag, reme_tag);
    QString sensor_path = settings.value(sensor_path_tag).toString();
    success = success && REME_SUCCESS(reme_sensor_create(_c, sensor_path.toStdString().c_str(), true, &_s));
    success = success && REME_SUCCESS(reme_sensor_open(_c, _s));
   
    if (success)
    {
      int w, h;
      bool supports_depth, supports_aux;

      reme_options_t o;
      reme_options_create(_c, &o);
      reme_sensor_bind_capture_options(_c, _s, o);

      // AUXILIARY image
      reme_options_get_bool(_c, o, "frame_info.supports_aux", &supports_aux);
      if (supports_aux) {
        reme_options_get_int(_c, o, "frame_info.aux_size.width", &w);
        reme_options_get_int(_c, o, "frame_info.aux_size.height", &h);
      }
      _rgb_size = supports_aux ? new QSize(w, h) : 0;
      emit rgb_size(_rgb_size);
      
      // DEPTH image
      reme_options_get_bool(_c, o, "frame_info.supports_depth", &supports_depth);
      if (supports_depth) {
        reme_options_get_int(_c, o, "frame_info.depth_size.width", &w);
        reme_options_get_int(_c, o, "frame_info.depth_size.height", &h);
      }
      _depth_size = supports_depth ? new QSize(w, h) : 0;
      emit depth_size(_depth_size);
    }

    _has_sensor = success;
    return _has_sensor;
  }

  bool reme_resource_manager::apply_license() {
    bool success;

    reme_license_t l;
    success = REME_SUCCESS(reme_license_create(_c, &l));
    
    // Set licence
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, profactor_tag, reme_tag);
    QString licence_file = settings.value(license_file_tag, license_file_default_tag).toString();    
    reme_error_t error = reme_license_authenticate(_c, l, licence_file.toStdString().c_str());
    if (error == REME_ERROR_INVALID_LICENSE)
      success = false;
    else if (error == REME_ERROR_UNSPECIFIED) 
      success = false;
    
    return success;
  }

  bool reme_resource_manager::compile_context()
  {
    bool success = true;

    // reload settings
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, profactor_tag, reme_tag);

    // Create empty options binding
    reme_options_t o;
    success = success && REME_SUCCESS(reme_options_create(_c, &o));
    success = success && REME_SUCCESS(reme_context_bind_compile_options(_c, o));

    // load options if config_path already set
    std::string path = settings.value(config_path_tag, config_path_default_tag).toString().toStdString();
    if (path != config_path_default_tag) {
      success = success && REME_SUCCESS(reme_options_load_from_file(_c, o, path.c_str()));
    }

    // apply selected opencl_device
    int device_id = settings.value(opencl_device_tag, opencl_device_default_tag).toInt();
    std::stringstream str_stream;
    str_stream << device_id;
    success = success && REME_SUCCESS(reme_options_set(_c, o, devcice_id_tag, str_stream.str().c_str()));

    // Compile for OpenCL device using modified options
    success = success && REME_SUCCESS(reme_context_compile(_c));

    if (!_has_volume) {
      success = success && REME_SUCCESS(reme_volume_create(_c, &_v));
      _has_volume = true;
    }

    _has_compiled_context = success;
    return _has_compiled_context;
  }

  reme_calibrator_t reme_resource_manager::new_calibrator() const {
    if (_has_compiled_context && _has_sensor) {
      reme_calibrator_t calib;
      reme_calibrator_create(_c, &calib);
      return calib;
    }
    else 
      return REME_ERROR_UNSPECIFIED;
  }

  void reme_resource_manager::destroy_calibrator(reme_calibrator_t calib) {
    if (_has_compiled_context)
      reme_calibrator_destroy(_c, &calib);
  }

  reme_image_t reme_resource_manager::new_image() const {
    if (_has_compiled_context) {
      reme_image_t img;
      reme_image_create(_c, &img);
      return img;
    }
    else 
      return REME_ERROR_UNSPECIFIED;
  }

  void reme_resource_manager::destroy_image(reme_image_t img) {
    reme_image_destroy(_c, &img);
  }

  const reme_context_t reme_resource_manager::context() const{
    return _c;
  }

  const reme_sensor_t reme_resource_manager::sensor() const{
    return _s;
  }

  const reme_volume_t reme_resource_manager::volume() const{
    return _v;
  }

  const QSize *reme_resource_manager::rgb_size() const {
    return _rgb_size;
  }

  const QSize *reme_resource_manager::depth_size() const {
    return _depth_size;
  }
}