# microbench optimisation

1. Build qjs in release mode with `zig build --release=fast`.
2. Run the microbench to capture baseline measures `./zig-out/bin/qjs tests/microbench.js > microbench-before.txt`
3. From the results select a single target for optimisation.
4. *CRITICAL* Implement your optimisation behind a configuration flag e.g. `-Dsome-example-flag=true`.
5. Rebuild qjs in release mode with your feature enabled e.g. `zig build --release=fast -Dsome-example-flag=true`.
6. Run the microbench to capture new measures `./zig-out/bin/qjs tests/microbench.js > microbench-after.txt`.
7. Compare values in each microbench output.
7.a. If performance has degraded significantly, rework or throw away.
7.b. If performance has improved significantly, document the new configuration the existing `./OPTIMISATION-README.md`.
