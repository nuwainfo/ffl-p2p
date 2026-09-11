#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Mark wheels as platform-specific when they include the built native extension."""

from setuptools import Distribution, setup


class NativeDistribution(Distribution):
    def has_ext_modules(self):
        return True


setup(distclass=NativeDistribution)
