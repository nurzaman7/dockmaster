# Copyright (c) 2026 Nurzaman
# SPDX-License-Identifier: Apache-2.0
#
# Sphinx configuration. Build with:  sphinx-build -W -b html doc doc/_build/html

project = "Dockmaster"
author = "Nurzaman"
copyright = "2026, Nurzaman"
release = "0.1.0"

extensions = ["myst_parser", "sphinxcontrib.mermaid"]
myst_enable_extensions = ["colon_fence"]
myst_fence_as_directive = ["mermaid"]
source_suffix = {".rst": "restructuredtext", ".md": "markdown"}
exclude_patterns = ["_build", "Doxyfile", "requirements.txt"]

html_theme = "sphinx_rtd_theme"
html_title = "Dockmaster"
html_show_sourcelink = False
