# hyprtester integration test

`plugins/layoutrestore.cpp` is the end-to-end test: it loads the built plugin into the hyprtester
compositor, saves a window's layout, kills and respawns the window, restores, and asserts the
window came back on its saved monitor/workspace with its saved geometry and floating state.

It has **not been run to completion locally** — the test environment cannot give Hyprland a DRM
seat or allocate a GBM buffer, so the nested compositor dies before the test body executes. See
the repository issue for the exact failure and how to run it where a GPU is available.

To run it, drop the files into a Hyprland checkout:

```sh
cp hyprtester/plugins/layoutrestore.cpp <hyprland>/hyprtester/src/tests/plugins/
cp hyprtester/plugins/tests.hpp        <hyprland>/hyprtester/src/tests/plugins/
```

and add to `hyprtester/src/main.cpp`:

```cpp
#include "tests/plugins/tests.hpp"
...
std::ranges::copy(pluginsTestCases, std::back_inserter(requestedTestCases));
```

then `hyprtester pluginLayoutRestoreSaveAndRestore`.
