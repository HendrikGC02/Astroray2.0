# SPDX-License-Identifier: Apache-2.0
set(C --Ofast-compile max)
set(D --split-compile 0 --relocatable-device-code=true)
target_compile_options(t PRIVATE /fp:fast)
# -use_fast_math in a comment is still a finding
