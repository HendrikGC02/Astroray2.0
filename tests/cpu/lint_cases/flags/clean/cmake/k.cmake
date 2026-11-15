# SPDX-License-Identifier: Apache-2.0
# no relocatable code, no fast math: the words are fine, the flags are not.
set(X -frdc-like --fmad=false /fp:precise)
