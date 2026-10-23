#include <iostream>
#include <string>
#include <vector>

#include "mc/diagnostics.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " #condition "  (line " << __LINE__ << ")\n"; \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main() {
    using cl::mc::analyze_run;

    auto summary = analyze_run(
        0, {"[INFO] Setting user: Player",
            "[ERROR] Failed to fetch user properties: Status: 401",
            "[ERROR] Realms authentication error ... Failed to parse into SignedJWT: 0"});
    CHECK(summary.normal);
    CHECK(summary.severity == "info");
    CHECK(summary.detail.find("离线") != std::string::npos);

    summary = analyze_run(1, {"java.lang.OutOfMemoryError: Java heap space"});
    CHECK(!summary.normal);
    CHECK(summary.title.find("内存") != std::string::npos);
    CHECK(!summary.suggestions.empty());

    summary = analyze_run(
        -1, {"[ForgeModLoader] Caught exception from ArmorStatusHUD",
             "java.lang.ArrayIndexOutOfBoundsException: 0"});
    CHECK(summary.title == "模组加载失败");
    CHECK(summary.detail.find("ArmorStatusHUD") != std::string::npos);
    CHECK(summary.severity == "error");

    summary = analyze_run(
        1, {"Module named cpw.mods.bootstraplauncher was already on the JVMs module path"});
    CHECK(summary.title.find("启动器") != std::string::npos);

    summary = analyze_run(1, {"java.lang.UnsupportedClassVersionError: foo"});
    CHECK(summary.title.find("Java") != std::string::npos);

    summary = analyze_run(1, {"Missing or unsupported mandatory dependencies:"});
    CHECK(summary.title.find("依赖") != std::string::npos);

    summary = analyze_run(1, {"java.lang.UnsatisfiedLinkError: lwjgl.dll"});
    CHECK(summary.title.find("原生库") != std::string::npos);

    summary = analyze_run(1, {"EXCEPTION_ACCESS_VIOLATION"});
    CHECK(summary.title.find("虚拟机") != std::string::npos);

    // 多条发现：内存结论里带上启动参数，且 finding 结构可用。
    cl::mc::AnalysisContext context;
    context.max_memory_mb = 2048;
    context.java_major = 17;
    summary = analyze_run(
        1, {"[12:34:56] [main/ERROR]: java.lang.OutOfMemoryError: Java heap space"},
        context);
    CHECK(summary.findings.size() == 1);
    CHECK(summary.findings[0].id == "memory.heap");
    CHECK(summary.detail.find("2048") != std::string::npos);
    CHECK(summary.findings[0].time.find("12:34:56") != std::string::npos);

    summary = analyze_run(
        1,
        {"java.lang.UnsupportedClassVersionError: foo has been compiled by a "
         "more recent version of the Java Runtime (class file version 65.0)"},
        context);
    CHECK(summary.findings[0].id == "java.class_version");
    CHECK(summary.detail.find("Java 21") != std::string::npos);
    CHECK(summary.detail.find("Java 17") != std::string::npos);

    // Forge 的缺失依赖可以解析出缺失模组和请求者。
    summary = analyze_run(
        1,
        {"Missing or unsupported mandatory dependencies:",
         "Mod ID: 'fabric-api', Requested by: 'sodium', Expected range: '[0.90,)'"});
    CHECK(summary.findings[0].id == "dependency.missing");
    CHECK(summary.findings[0].mod_a == "fabric-api");
    CHECK(summary.findings[0].mod_b == "sodium");
    CHECK(summary.detail.find("fabric-api") != std::string::npos);

    // Fabric 风格的 requires 语句。
    summary = analyze_run(
        1,
        {"Mod 'sodium' requires 'fabric-api' version 0.90+, which is missing!"});
    CHECK(summary.findings[0].mod_a == "fabric-api");
    CHECK(summary.findings[0].mod_b == "sodium");

    // 已知冲突组合。
    summary = analyze_run(
        1, {"[minecraft/INFO] Loading OptiFine",
            "[minecraft/INFO] Loading Sodium",
            "java.lang.NoSuchMethodError: net.minecraft.client.renderer"});
    bool has_optifine_conflict = false;
    for (const auto& finding : summary.findings) {
        if (finding.id == "conflict.optifine_render") {
            has_optifine_conflict = true;
            CHECK(finding.mod_a == "OptiFine");
            CHECK(finding.mod_b.find("Sodium") != std::string::npos);
        }
    }
    CHECK(has_optifine_conflict);

    // 正常退出时不要仅凭模组名同时出现就报冲突。
    summary = analyze_run(0, {"[INFO] Loading OptiFine", "[INFO] Loading Sodium"});
    CHECK(summary.normal);
    for (const auto& finding : summary.findings) {
        CHECK(finding.id != "conflict.optifine_render");
    }

    // 崩溃报告行要能作为独立特征，并带上日志时间。
    summary = analyze_run(
        1,
        {"[20:41:07] [main/ERROR]: The game crashed whilst initializing game",
         "Description: Initializing game"});
    bool has_crash_report = false;
    for (const auto& finding : summary.findings) {
        if (finding.id == "crash.report") {
            has_crash_report = true;
            CHECK(finding.time == "20:41:07");
            CHECK(finding.detail.find("Initializing game") != std::string::npos);
        }
    }
    CHECK(has_crash_report);

    // 未知特征要标记 unknown，供前端提供导出按钮。
    summary = analyze_run(1, {"something odd happened"});
    CHECK(summary.unknown);
    CHECK(summary.findings.empty());

    summary = analyze_run(1, {"something odd happened"});
    CHECK(!summary.normal);
    CHECK(summary.title.find("异常") != std::string::npos);

    summary = analyze_run(0, {"Stopping!"});
    CHECK(summary.normal);
    CHECK(summary.severity == "info");

    std::cout << "diagnostics_test: all checks passed\n";
    return 0;
}
