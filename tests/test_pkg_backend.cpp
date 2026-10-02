// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/pkg/client/pkg_backend.hpp"
#include "modules/bdg/dev/pkg/client/pkg_process.hpp"

using namespace bdg::wish::pkg;

namespace {

using argv_t = std::vector<std::string>;

// ── run_pkg_cli() ───────────────────────────────────────────────────────────
//
// Exercised with stub programs (never a package manager) so these pass on
// any machine.

TEST(PkgProcessTest, RunsAWholeArgvAndReportsExitCodes) {
  auto r = run_pkg_cli({"printf", "%s|%s", "a", "b c"});
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "a|b c");
  EXPECT_EQ(run_pkg_cli({"false"}).exit_code, 1);
}

TEST(PkgProcessTest, MissingProgramIsExitCodeMinusOne) {
  auto r = run_pkg_cli({"definitely-not-a-package-manager-xyzzy", "--version"});
  EXPECT_EQ(r.exit_code, -1) << "run_pkg() tells 'not installed' apart from 'failed' by this";
  EXPECT_FALSE(r.stderr_text.empty());
  EXPECT_EQ(run_pkg_cli({}).exit_code, -1);
}

TEST(PkgProcessTest, AFailedSpawnDoesNotBreakTheNextOne) {
  // run_pkg() probes for programs that are usually absent (pkexec, the other
  // package managers) and then carries on in the same process.
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(run_pkg_cli({"definitely-not-a-package-manager-xyzzy"}).exit_code, -1);
  auto r = run_pkg_cli({"printf", "still works"});
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "still works");
}

// ── managers / elevation ────────────────────────────────────────────────────

TEST(PkgBackendTest, ManagersRoundTripByName) {
  for (manager m : all_managers())
    EXPECT_EQ(manager_from_name(manager_name(m)), m);
  EXPECT_FALSE(manager_from_name("yum"));
  EXPECT_FALSE(manager_from_name(""));
  EXPECT_EQ(manager_names(), "apt, dnf, pacman, brew");
  EXPECT_EQ(elevation_from_name("pkexec"), elevation::pkexec);
  EXPECT_FALSE(elevation_from_name("auto")) << "auto is resolved by probing, not a mode";
}

TEST(PkgBackendTest, EveryManagerHasEveryCommand) {
  for (manager m : all_managers()) {
    SCOPED_TRACE(manager_name(m));
    EXPECT_FALSE(probe_command(m).argv.empty());
    EXPECT_FALSE(list_command(m).argv.empty());
    EXPECT_FALSE(outdated_command(m).argv.empty());
    EXPECT_FALSE(search_command(m, "x").argv.empty());
    EXPECT_FALSE(show_commands(m, "x").empty());
    EXPECT_FALSE(files_command(m, "x").argv.empty());
    EXPECT_FALSE(install_command(m, {"x"}).argv.empty());
    EXPECT_FALSE(remove_command(m, "x").argv.empty());
    EXPECT_FALSE(upgrade_command(m, "x").argv.empty());
    EXPECT_FALSE(reinstall_command(m, "x").argv.empty());
    EXPECT_FALSE(upgrade_all_command(m).argv.empty());
    EXPECT_FALSE(refresh_index_command(m).argv.empty());
    // Reading never needs root; changing the system does, except with brew.
    EXPECT_FALSE(list_command(m).needs_root);
    EXPECT_FALSE(search_command(m, "x").needs_root);
    EXPECT_EQ(install_command(m, {"x"}).needs_root, m != manager::brew);
    EXPECT_EQ(remove_command(m, "x").needs_root, m != manager::brew);
  }
}

TEST(PkgBackendTest, MutatingCommandsNeverPrompt) {
  EXPECT_EQ(install_command(manager::apt, {"htop", "curl"}).argv,
            (argv_t{"apt-get", "install", "-y", "--", "htop", "curl"}));
  EXPECT_EQ(remove_command(manager::dnf, "htop").argv, (argv_t{"dnf", "remove", "-y", "--", "htop"}));
  EXPECT_EQ(install_command(manager::pacman, {"htop"}).argv,
            (argv_t{"pacman", "-S", "--noconfirm", "--needed", "--", "htop"}));
  EXPECT_EQ(upgrade_command(manager::apt, "htop").argv,
            (argv_t{"apt-get", "install", "-y", "--only-upgrade", "--", "htop"}));
  EXPECT_EQ(install_command(manager::brew, {"wget"}).argv, (argv_t{"brew", "install", "wget"}));
}

TEST(PkgBackendTest, ElevationWrapsOnlyCommandsThatNeedRoot) {
  const auto install = install_command(manager::dnf, {"htop"});
  EXPECT_EQ(elevated(manager::dnf, install, elevation::sudo),
            (argv_t{"sudo", "-n", "dnf", "install", "-y", "--", "htop"}));
  EXPECT_EQ(elevated(manager::dnf, install, elevation::pkexec),
            (argv_t{"pkexec", "dnf", "install", "-y", "--", "htop"}));
  EXPECT_EQ(elevated(manager::dnf, install, elevation::none), install.argv);

  // apt must be told not to ask questions, through `env` since sudo and
  // pkexec both reset the environment.
  EXPECT_EQ(elevated(manager::apt, install_command(manager::apt, {"htop"}), elevation::sudo),
            (argv_t{"sudo", "-n", "env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "--", "htop"}));

  const auto list = list_command(manager::apt);
  EXPECT_EQ(elevated(manager::apt, list, elevation::sudo), list.argv) << "a read-only command runs as the user";
  EXPECT_EQ(elevated(manager::brew, install_command(manager::brew, {"wget"}), elevation::sudo),
            (argv_t{"brew", "install", "wget"}))
      << "brew refuses to run as root";
}

// ── installed listings ──────────────────────────────────────────────────────

TEST(PkgBackendTest, ParsesDpkgQueryKeepingOnlyInstalledPackages) {
  auto p = parse_installed(
      manager::apt,
      "adduser\t3.153ubuntu1\tii \tadd and remove users and groups\n"
      "libc6:amd64\t2.39-0ubuntu8\tii \tGNU C Library: Shared libraries\n"
      "old-thing\t1.0\trc \tremoved, configuration left behind\n"
      "broken\t1.0\tiU \thalf installed\n");
  ASSERT_EQ(p.size(), 2u);
  EXPECT_EQ(p[0].name, "adduser");
  EXPECT_EQ(p[0].version, "3.153ubuntu1");
  EXPECT_EQ(p[0].description, "add and remove users and groups");
  EXPECT_EQ(p[1].name, "libc6:amd64");
}

TEST(PkgBackendTest, ParsesRpmPacmanAndBrewListings) {
  auto rpm = parse_installed(manager::dnf, "bash\t5.2.26-3.fc40\tThe GNU Bourne Again shell\nzlib\t1.3-1.fc40\t\n");
  ASSERT_EQ(rpm.size(), 2u);
  EXPECT_EQ(rpm[0].version, "5.2.26-3.fc40");
  EXPECT_EQ(rpm[0].description, "The GNU Bourne Again shell");
  EXPECT_EQ(rpm[1].description, "");

  auto pac = parse_installed(manager::pacman, "linux 6.8.1.arch1-1\nzlib 1:1.3.1-1\n\n");
  ASSERT_EQ(pac.size(), 2u);
  EXPECT_EQ(pac[1].name, "zlib");
  EXPECT_EQ(pac[1].version, "1:1.3.1-1");

  auto brew = parse_installed(manager::brew, "wget 1.21.4\nopenssl@3 3.2.0 3.3.1\n");
  ASSERT_EQ(brew.size(), 2u);
  EXPECT_EQ(brew[1].name, "openssl@3");
  EXPECT_EQ(brew[1].version, "3.3.1") << "the newest of several kept versions";
}

// ── outdated listings ───────────────────────────────────────────────────────

TEST(PkgBackendTest, ParsesOutdatedListings) {
  auto apt = parse_outdated(
      manager::apt,
      "Listing...\n"
      "htop/noble-updates 3.3.0-4build1 amd64 [upgradable from: 3.3.0-4]\n"
      "libc6/noble-updates,noble-security 2.39-0ubuntu8.3 amd64 [upgradable from: 2.39-0ubuntu8]\n");
  ASSERT_EQ(apt.size(), 2u);
  EXPECT_EQ(apt[0].name, "htop");
  EXPECT_EQ(apt[0].latest, "3.3.0-4build1");
  EXPECT_EQ(apt[1].name, "libc6");

  auto dnf = parse_outdated(
      manager::dnf,
      "Last metadata expiration check: 0:12:01 ago on Fri 02 Oct 2026.\n\n"
      "bash.x86_64                 5.2.32-1.fc40        updates\n"
      "python3.12.x86_64           3.12.5-1.fc40        updates\n"
      "Obsoleting Packages\n"
      "kernel.x86_64               6.9.0-1.fc40         updates\n");
  ASSERT_EQ(dnf.size(), 2u) << "the Obsoleting section is not part of the list";
  EXPECT_EQ(dnf[0].name, "bash");
  EXPECT_EQ(dnf[0].latest, "5.2.32-1.fc40");
  EXPECT_EQ(dnf[1].name, "python3.12") << "only the architecture is stripped";

  auto pac = parse_outdated(manager::pacman, "linux 6.8.1.arch1-1 -> 6.8.2.arch1-1\n");
  ASSERT_EQ(pac.size(), 1u);
  EXPECT_EQ(pac[0].latest, "6.8.2.arch1-1");

  auto brew = parse_outdated(manager::brew, "wget (1.21.4) < 1.24.5\nopenssl@3 (3.2.0, 3.2.1) < 3.3.1\napp (1.0) != 1.1\n");
  ASSERT_EQ(brew.size(), 3u);
  EXPECT_EQ(brew[1].name, "openssl@3");
  EXPECT_EQ(brew[1].latest, "3.3.1");
  EXPECT_EQ(brew[2].latest, "1.1");

  EXPECT_TRUE(parse_outdated(manager::apt, "Listing...\n").empty());
}

TEST(PkgBackendTest, OutdatedExitCodesThatAreNotFailures) {
  EXPECT_TRUE(outdated_succeeded(manager::dnf, 100)) << "dnf: updates are available";
  EXPECT_FALSE(outdated_succeeded(manager::dnf, 1));
  EXPECT_TRUE(outdated_succeeded(manager::pacman, 1)) << "pacman: nothing to update";
  EXPECT_FALSE(outdated_succeeded(manager::apt, 100));
  EXPECT_TRUE(outdated_succeeded(manager::brew, 0));
}

// ── search results ──────────────────────────────────────────────────────────

TEST(PkgBackendTest, ParsesSearchResults) {
  auto apt = parse_search(manager::apt, "htop - interactive processes viewer\nlibx-y - a - b\n");
  ASSERT_EQ(apt.size(), 2u);
  EXPECT_EQ(apt[0].name, "htop");
  EXPECT_EQ(apt[0].description, "interactive processes viewer");
  EXPECT_EQ(apt[1].name, "libx-y");
  EXPECT_EQ(apt[1].description, "a - b");

  // dnf 4 and dnf 5 layouts.
  auto dnf4 = parse_search(
      manager::dnf,
      "======================= Name Exactly Matched: htop =======================\n"
      "htop.x86_64 : Interactive process viewer\n");
  ASSERT_EQ(dnf4.size(), 1u);
  EXPECT_EQ(dnf4[0].name, "htop");
  EXPECT_EQ(dnf4[0].description, "Interactive process viewer");
  auto dnf5 = parse_search(manager::dnf, "Matched fields: name (exact)\n htop.x86_64\tInteractive process viewer\n");
  ASSERT_EQ(dnf5.size(), 1u);
  EXPECT_EQ(dnf5[0].name, "htop");
  EXPECT_EQ(dnf5[0].description, "Interactive process viewer");

  auto pac = parse_search(
      manager::pacman,
      "extra/htop 3.3.0-3 [installed]\n    Interactive process viewer\ncore/zlib 1:1.3.1-1\n    Compression library\n");
  ASSERT_EQ(pac.size(), 2u);
  EXPECT_EQ(pac[0].name, "htop");
  EXPECT_EQ(pac[0].version, "3.3.0-3");
  EXPECT_EQ(pac[0].description, "Interactive process viewer");
  EXPECT_EQ(pac[1].description, "Compression library");

  auto brew = parse_search(manager::brew, "==> Formulae\nwget\nwgetpaste\n\n==> Casks\nwget-gui\n");
  ASSERT_EQ(brew.size(), 3u);
  EXPECT_EQ(brew[2].name, "wget-gui");
}

// ── validation / messages ───────────────────────────────────────────────────

TEST(PkgBackendTest, PackageNamesCannotBeOptions) {
  EXPECT_TRUE(is_valid_package_name("htop"));
  EXPECT_TRUE(is_valid_package_name("libc6:amd64"));
  EXPECT_TRUE(is_valid_package_name("g++"));
  EXPECT_TRUE(is_valid_package_name("openssl@3"));
  EXPECT_TRUE(is_valid_package_name("homebrew/cask/firefox"));
  EXPECT_FALSE(is_valid_package_name(""));
  EXPECT_FALSE(is_valid_package_name("-y"));
  EXPECT_FALSE(is_valid_package_name("--root=/elsewhere"));
  EXPECT_FALSE(is_valid_package_name("a b"));
  EXPECT_FALSE(is_valid_package_name("a;rm"));
  EXPECT_FALSE(is_valid_package_name("$(x)"));
  EXPECT_EQ(split_names("  htop \t curl\n"), (argv_t{"htop", "curl"}));
  EXPECT_EQ(base_name("libc6:amd64"), "libc6");
  EXPECT_EQ(base_name("htop"), "htop");
}

TEST(PkgBackendTest, ErrorSummaryExplainsARefusedPrivilegeRequest) {
  auto sudo = error_summary(elevation::sudo, "sudo: a password is required\n");
  EXPECT_NE(sudo.find("sudo -v"), std::string::npos) << sudo;
  EXPECT_NE(sudo.find("pkexec"), std::string::npos) << sudo;
  EXPECT_NE(error_summary(elevation::sudo, "sudo: interactive authentication is required\n").find("sudo -v"),
            std::string::npos);
  auto polkit = error_summary(elevation::pkexec, "Error executing command as another user: Not authorized\n");
  EXPECT_NE(polkit.find("not granted"), std::string::npos) << polkit;
}

TEST(PkgBackendTest, ErrorSummaryKeepsTheManagersOwnErrorLines) {
  EXPECT_EQ(error_summary(elevation::none, "Reading package lists...\nE: Unable to locate package nope\n"),
            "E: Unable to locate package nope");
  EXPECT_EQ(error_summary(elevation::none, "error: target not found: nope\n"), "error: target not found: nope");
  EXPECT_EQ(error_summary(elevation::none, "something else\n"), "something else");
}

} // namespace
