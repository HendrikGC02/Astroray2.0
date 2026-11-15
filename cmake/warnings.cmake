# SPDX-License-Identifier: Apache-2.0
# Our targets only; vendored headers are SYSTEM includes.
function(astroray_target_warnings target)
  target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
endfunction()
