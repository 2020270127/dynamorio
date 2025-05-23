# **********************************************************
# Copyright (c) 2021-2025 Google, Inc.    All rights reserved.
# **********************************************************

# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
#
# * Neither the name of Google, Inc. nor the names of its contributors may be
#   used to endorse or promote products derived from this software without
#   specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
# DAMAGE.

cmake_minimum_required(VERSION 3.14)

include(../../make/policies.cmake NO_POLICY_SCOPE)

# DynamoRIO Machine State Comparison Library

# Avoiding libc reduces lib size (i#714)
set(DynamoRIO_USE_LIBC OFF)

set(srcs drstatecmp.c)

set(srcs_static ${srcs})

add_library(drstatecmp SHARED ${srcs})
configure_extension(drstatecmp OFF OFF)
use_DynamoRIO_extension(drstatecmp drmgr)
use_DynamoRIO_extension(drstatecmp drreg)

add_library(drstatecmp_static STATIC ${srcs_static})
configure_extension(drstatecmp_static ON OFF)
use_DynamoRIO_extension(drstatecmp_static drmgr_static)
use_DynamoRIO_extension(drstatecmp_static drreg_static)

add_library(drstatecmp_drstatic STATIC ${srcs_static})
configure_extension(drstatecmp_drstatic ON ON)
use_DynamoRIO_extension(drstatecmp_drstatic drmgr_drstatic)
use_DynamoRIO_extension(drstatecmp_drstatic drreg_drstatic)

install_ext_header(drstatecmp.h)
