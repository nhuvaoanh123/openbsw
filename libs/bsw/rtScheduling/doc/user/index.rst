Using rtScheduling
==================

.. code-block:: cpp

   // Single-file manifest — the only place your schedule lives.
   constexpr std::array<rtSched::EntityDecl, 3> MANIFEST{{
       { "safety", TASK_SAFETY,
         { /* cycle */ 5000, /* wcet */ 700, /* deadline */ 5000, /* phase */ 0 },
         rtSched::Criticality::ASIL_D, rtSched::OverrunPolicy::Safety },
       { "control", TASK_APP,
         { 10000, 1500, 10000, 0 },
         rtSched::Criticality::ASIL_B, rtSched::OverrunPolicy::LogAndCount },
       { "telemetry", TASK_BG,
         { 100000, 5000, 100000, 0 },
         rtSched::Criticality::QM, rtSched::OverrunPolicy::LogAndCount },
   }};

   // Compile-time gate. Fails the build if the manifest goes infeasible.
   static_assert(rtSched::analyze_with_rta(rtSched::view(MANIFEST))
                 == rtSched::Verdict::Ok, "schedule infeasible");

   // At startup, inside app::startApp(), replace every per-system
   // scheduleAtFixedRate call with one arm() call:
   void startApp() {
       rtSched::bridge::install();
       std::array<rtSched::Binding, 3> bindings{{
           { &MANIFEST[0], &safety_runnable,   &safety_timeout,   &safety_stats },
           { &MANIFEST[1], &control_runnable,  &control_timeout,  &control_stats },
           { &MANIFEST[2], &telemetry_runnable,&telemetry_timeout,&telemetry_stats },
       }};
       auto const rc = rtSched::arm_manifest(bindings);
       // rc != Ok → transition to safe state; feasibility has already been
       // proven at build time, so only implementation or binding errors
       // (null pointer, capacity exceeded) can arrive here.
   }

Auditing at runtime
-------------------

.. code-block:: cpp

   // Dump a complete feasibility report to any buffer. Suitable for UART,
   // a file writer, or a console command.
   std::string buf;
   rtSched::json::write_report_from_registry(buf);
   // buf now carries a `rt-sched/1` JSON object with per-entity decls,
   // per-context L&L / hyperbolic / RTA verdicts, response times,
   // ASIL-criticality utilization breakdown, and per-entity stats.
