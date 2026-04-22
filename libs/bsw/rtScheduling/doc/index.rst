rtScheduling
============

Overview
--------

The ``rtScheduling`` module provides a declarative real-time scheduling layer
on top of ``async``. It addresses the ask in issue `#370 "Facilitate real-time
operation" <https://github.com/eclipse-openbsw/openbsw/issues/370>`_ by making
the schedule of an application visible as a single, inspectable manifest, by
validating feasibility at compile time, and by enforcing declared worst-case
execution times at runtime.

Highlights
----------

- **One manifest, one gate.** All periodic entities (cycle, WCET, deadline,
  criticality) are declared in a ``std::array<EntityDecl, N>``. A single call
  to ``arm()`` replaces every scattered ``async::scheduleAtFixedRate`` call.

- **Compile-time feasibility.** ``analyze_with_rta`` is ``constexpr``; placing
  it in a ``static_assert`` turns the build into an ASIL-grade feasibility
  gate. An edit that over-subscribes a task context fails the build, not the
  deployment.

- **Exact response-time analysis.** Joseph-Pandya RTA with declaration-order
  tie-breaking for equal periods, matching the FIFO-within-priority behaviour
  of FreeRTOS, ThreadX, and OSEK. Blocking term supported per entity (PIP /
  PCP). Both sufficient tests (Liu-Layland 1973, Bini-Buttazzo-Buttazzo 2003)
  are also exposed for reference.

- **Runtime WCET enforcement.** ``MonitoredRunnable`` wraps user runnables,
  samples execution time, bumps per-entity stats, and fires a user-supplied
  overrun handler when exec_us exceeds the declared WCET.

- **JSON export.** ``rtSched::json::write_report`` emits a stable, versioned
  schedule report (``rt-sched/1``) suitable for consumption by the reference
  audit GUI or any offline analyser.

.. toctree::
   :maxdepth: 1

   user/index
