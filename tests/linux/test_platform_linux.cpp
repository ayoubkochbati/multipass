/*
 * Copyright (C) 2019-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "tests/fake_handle.h"
#include "tests/mock_environment_helpers.h"
#include "tests/mock_process_factory.h"
#include "tests/mock_settings.h"
#include "tests/test_with_mocked_bin_path.h"

#include <src/platform/backends/libvirt/libvirt_virtual_machine_factory.h>
#include <src/platform/backends/libvirt/libvirt_wrapper.h>
#include <src/platform/backends/lxd/lxd_virtual_machine_factory.h>
#include <src/platform/backends/qemu/qemu_virtual_machine_factory.h>

#include <multipass/constants.h>
#include <multipass/exceptions/autostart_setup_exception.h>
#include <multipass/exceptions/settings_exceptions.h>
#include <multipass/platform.h>

#include <QDir>
#include <QFile>
#include <QString>

#include <scope_guard.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>

namespace mp = multipass;
namespace mpt = multipass::test;
using namespace testing;

namespace
{
const auto backend_path = QStringLiteral("/tmp");

void setup_driver_settings(const QString& driver)
{
    auto& expectation = EXPECT_CALL(mpt::MockSettings::mock_instance(), get(Eq(mp::driver_key)));
    if (!driver.isEmpty())
        expectation.WillRepeatedly(Return(driver));
}

// hold on to return until the change is to be discarded
auto temporarily_change_env(const char* var_name, QByteArray var_value)
{
    auto guard = sg::make_scope_guard([var_name, var_save = qgetenv(var_name)]() { qputenv(var_name, var_save); });
    qputenv(var_name, var_value);

    return guard;
}

template <typename Guards>
struct autostart_test_record
{
    autostart_test_record(const QDir& autostart_dir, const QString& autostart_filename,
                          const QString& autostart_contents, Guards&& guards)
        : autostart_dir{autostart_dir},
          autostart_filename{autostart_filename},
          autostart_contents{autostart_contents},
          guards{std::forward<Guards>(guards)} // this should always move, since scope guards are not copyable
    {
    }

    QDir autostart_dir;
    QString autostart_filename;
    QString autostart_contents;
    Guards guards;
};

struct autostart_test_setup_error : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

auto setup_autostart_desktop_file_test()
{
    QDir test_dir{QDir::temp().filePath(QString{"%1_%2"}.arg(mp::client_name, "autostart_test"))};
    if (test_dir.exists() || QFile{test_dir.path()}.exists()) // avoid touching this at all if it already exists
        throw autostart_test_setup_error{fmt::format("Test dir or file already exists: {}", test_dir.path())}; /*
                           use exception to abort the test, since ASSERTS only work in void-returning functions */

    // Now mock filesystem tree and environment, reverting when done

    auto guard_fs = sg::make_scope_guard([test_dir]() mutable {
        test_dir.removeRecursively(); // succeeds if not there
    });

    QDir data_dir{test_dir.filePath("data")};
    QDir config_dir{test_dir.filePath("config")};
    auto guard_home = temporarily_change_env("HOME", "hide/me");
    auto guard_xdg_config = temporarily_change_env("XDG_CONFIG_HOME", config_dir.path().toLatin1());
    auto guard_xdg_data = temporarily_change_env("XDG_DATA_DIRS", data_dir.path().toLatin1());

    QDir mp_data_dir{data_dir.filePath(mp::client_name)};
    QDir autostart_dir{config_dir.filePath("autostart")};
    EXPECT_TRUE(mp_data_dir.mkpath("."));   // this is where the directories are actually created
    EXPECT_TRUE(autostart_dir.mkpath(".")); // idem

    const auto autostart_filename = mp::platform::autostart_test_data();
    const auto autostart_filepath = mp_data_dir.filePath(autostart_filename);
    const auto autostart_contents = "Exec=multipass.gui --autostarting\n";

    {
        QFile autostart_file{autostart_filepath};
        EXPECT_TRUE(autostart_file.open(QIODevice::WriteOnly)); // create desktop file to link against
        EXPECT_EQ(autostart_file.write(autostart_contents), qstrlen(autostart_contents));
    }

    auto guards = std::make_tuple(std::move(guard_home), std::move(guard_xdg_config), std::move(guard_xdg_data),
                                  std::move(guard_fs));
    return autostart_test_record{autostart_dir, autostart_filename, autostart_contents, std::move(guards)};
}

void check_autostart_file(const QDir& autostart_dir, const QString& autostart_filename,
                          const QString& autostart_contents)
{
    QFile f{autostart_dir.filePath(autostart_filename)};
    ASSERT_TRUE(f.exists());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));

    auto actual_contents = QString{f.readAll()};
    EXPECT_EQ(actual_contents, autostart_contents);
}

struct PlatformLinux : public mpt::TestWithMockedBinPath
{
    template <typename VMFactoryType>
    void aux_test_driver_factory(const QString& driver = QStringLiteral(""))
    {
        setup_driver_settings(driver);

        decltype(mp::platform::vm_backend("")) factory_ptr;
        EXPECT_NO_THROW(factory_ptr = mp::platform::vm_backend(backend_path););

        EXPECT_TRUE(dynamic_cast<VMFactoryType*>(factory_ptr.get()));
    }

    void with_minimally_mocked_libvirt(std::function<void()> test_contents)
    {
        mp::LibvirtWrapper libvirt_wrapper{""};
        test_contents();
    }

    mpt::UnsetEnvScope unset_env_scope{mp::driver_env_var};
    mpt::SetEnvScope disable_apparmor{"DISABLE_APPARMOR", "1"};
};

void simulate_ip(const mpt::MockProcess* process, const mp::ProcessState& produce_result,
                 const QByteArray& produce_output = {})
{
    ASSERT_EQ(process->program().toStdString(), "ip");

    // ip can be called with arguments "link show dev" or "address".
    const auto args = process->arguments();
    ASSERT_THAT(args.size(), Ge(1));
    if (args.size() == 1)
    {
        EXPECT_EQ(args.constFirst(), "address");
    }
    else
    {
        EXPECT_EQ(args.size(), 4);
        EXPECT_EQ(args[0], "link");
        EXPECT_EQ(args[1], "show");
        EXPECT_EQ(args[2], "dev");
    }

    EXPECT_CALL(*process, execute).WillOnce(Return(produce_result));
    if (produce_result.completed_successfully())
        EXPECT_CALL(*process, read_all_standard_output).WillOnce(Return(produce_output));
    else if (produce_result.exit_code)
        EXPECT_CALL(*process, read_all_standard_error).WillOnce(Return(produce_output));
    else
        ON_CALL(*process, read_all_standard_error).WillByDefault(Return(produce_output));
}

std::unique_ptr<mp::test::MockProcessFactory::Scope> inject_fake_ip_callback(const mp::ProcessState& ip_exit_status,
                                                                             const QByteArray& ip_output)
{
    std::unique_ptr<mp::test::MockProcessFactory::Scope> mock_factory_scope = mpt::MockProcessFactory::Inject();

    mock_factory_scope->register_callback(
        [&](mpt::MockProcess* process) { simulate_ip(process, ip_exit_status, ip_output); });

    return mock_factory_scope;
}

TEST_F(PlatformLinux, test_interpretation_of_winterm_setting_not_supported)
{
    for (const auto x : {"no", "matter", "what"})
        EXPECT_THROW(mp::platform::interpret_setting(mp::winterm_key, x), mp::InvalidSettingsException);
}

TEST_F(PlatformLinux, test_interpretation_of_unknown_settings_not_supported)
{
    for (const auto k : {"unimaginable", "katxama", "katxatxa"})
        for (const auto v : {"no", "matter", "what"})
            EXPECT_THROW(mp::platform::interpret_setting(k, v), mp::InvalidSettingsException);
}

TEST_F(PlatformLinux, test_empty_sync_winterm_profiles)
{
    EXPECT_NO_THROW(mp::platform::sync_winterm_profiles());
}

TEST_F(PlatformLinux, test_autostart_desktop_file_properly_placed)
{
    try
    {
        const auto& [autostart_dir, autostart_filename, autostart_contents, guards] =
            setup_autostart_desktop_file_test();

        ASSERT_FALSE(HasFailure()) << "autostart test setup failed";

        mp::platform::setup_gui_autostart_prerequisites();
        check_autostart_file(autostart_dir, autostart_filename, autostart_contents);
    }
    catch (autostart_test_setup_error& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(PlatformLinux, test_autostart_setup_replaces_wrong_link)
{
    try
    {
        const auto& [autostart_dir, autostart_filename, autostart_contents, guards] =
            setup_autostart_desktop_file_test();

        ASSERT_FALSE(HasFailure()) << "autostart test setup failed";

        const auto bad_filename = autostart_dir.filePath("wrong_file");
        QFile bad_file{bad_filename};

        EXPECT_TRUE(bad_file.open(QIODevice::WriteOnly)); // create desktop file to link against
        bad_file.write("bad contents");
        bad_file.close();

        bad_file.link(autostart_dir.filePath(autostart_filename)); // link to a bad file

        mp::platform::setup_gui_autostart_prerequisites();
        check_autostart_file(autostart_dir, autostart_filename, autostart_contents);
    }
    catch (autostart_test_setup_error& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(PlatformLinux, test_autostart_setup_replaces_broken_link)
{
    try
    {
        const auto& [autostart_dir, autostart_filename, autostart_contents, guards] =
            setup_autostart_desktop_file_test();

        ASSERT_FALSE(HasFailure()) << "autostart test setup failed";

        const auto bad_filename = autostart_dir.filePath("absent_file");
        QFile bad_file{bad_filename};
        ASSERT_FALSE(bad_file.exists());
        bad_file.link(autostart_dir.filePath(autostart_filename)); // link to absent file
        ASSERT_FALSE(bad_file.exists());

        mp::platform::setup_gui_autostart_prerequisites();
        check_autostart_file(autostart_dir, autostart_filename, autostart_contents);
    }
    catch (autostart_test_setup_error& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(PlatformLinux, test_autostart_setup_leaves_non_link_file_alone)
{
    try
    {
        const auto& [autostart_dir, autostart_filename, autostart_contents, guards] =
            setup_autostart_desktop_file_test();

        ASSERT_FALSE(HasFailure()) << "autostart test setup failed";

        const auto replacement_contents = "replacement contents";

        {
            QFile replacement_file{autostart_dir.filePath(autostart_filename)};
            ASSERT_FALSE(replacement_file.exists());
            ASSERT_TRUE(replacement_file.open(QIODevice::WriteOnly)); // create desktop file to link against
            ASSERT_EQ(replacement_file.write(replacement_contents), qstrlen(replacement_contents));
        }

        mp::platform::setup_gui_autostart_prerequisites();
        check_autostart_file(autostart_dir, autostart_filename, replacement_contents);
    }
    catch (autostart_test_setup_error& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(PlatformLinux, test_autostart_setup_fails_on_absent_desktop_target)
{
    const auto guard_xdg = temporarily_change_env("XDG_DATA_DIRS", "/dadgad/bad/dir");
    const auto guard_home = temporarily_change_env("HOME", "dadgbd/bad/too");

    EXPECT_THROW(mp::platform::setup_gui_autostart_prerequisites(), mp::AutostartSetupException);
}

TEST_F(PlatformLinux, test_default_qemu_driver_produces_correct_factory)
{
    aux_test_driver_factory<mp::QemuVirtualMachineFactory>();
}

TEST_F(PlatformLinux, test_explicit_qemu_driver_produces_correct_factory)
{
    aux_test_driver_factory<mp::QemuVirtualMachineFactory>("qemu");
}

TEST_F(PlatformLinux, test_libvirt_driver_produces_correct_factory)
{
    auto test = [this] { aux_test_driver_factory<mp::LibVirtVirtualMachineFactory>("libvirt"); };
    with_minimally_mocked_libvirt(test);
}

TEST_F(PlatformLinux, test_lxd_driver_produces_correct_factory)
{
    aux_test_driver_factory<mp::LXDVirtualMachineFactory>("lxd");
}

TEST_F(PlatformLinux, test_qemu_in_env_var_is_ignored)
{
    mpt::SetEnvScope env(mp::driver_env_var, "QEMU");
    auto test = [this] { aux_test_driver_factory<mp::LibVirtVirtualMachineFactory>("libvirt"); };
    with_minimally_mocked_libvirt(test);
}

TEST_F(PlatformLinux, test_libvirt_in_env_var_is_ignored)
{
    mpt::SetEnvScope env(mp::driver_env_var, "LIBVIRT");
    aux_test_driver_factory<mp::QemuVirtualMachineFactory>("qemu");
}

TEST_F(PlatformLinux, test_is_remote_supported_returns_true)
{
    EXPECT_TRUE(mp::platform::is_remote_supported("release"));
    EXPECT_TRUE(mp::platform::is_remote_supported("daily"));
    EXPECT_TRUE(mp::platform::is_remote_supported(""));
    EXPECT_TRUE(mp::platform::is_remote_supported("snapcraft"));
    EXPECT_TRUE(mp::platform::is_remote_supported("appliance"));
}

TEST_F(PlatformLinux, test_snap_returns_expected_default_address)
{
    const QByteArray base_dir{"/tmp"};
    const QByteArray snap_name{"multipass"};

    mpt::SetEnvScope env("SNAP_COMMON", base_dir);
    mpt::SetEnvScope env2("SNAP_NAME", snap_name);

    EXPECT_EQ(mp::platform::default_server_address(), fmt::format("unix:{}/multipass_socket", base_dir.toStdString()));
}

TEST_F(PlatformLinux, test_not_snap_returns_expected_default_address)
{
    const QByteArray snap_name{"multipass"};

    mpt::UnsetEnvScope unset_env("SNAP_COMMON");
    mpt::SetEnvScope env2("SNAP_NAME", snap_name);

    EXPECT_EQ(mp::platform::default_server_address(), fmt::format("unix:/run/multipass_socket"));
}

struct TestUnsupportedDrivers : public TestWithParam<QString>
{
};

TEST_P(TestUnsupportedDrivers, test_unsupported_driver)
{
    ASSERT_FALSE(mp::platform::is_backend_supported(GetParam()));

    setup_driver_settings(GetParam());
    EXPECT_THROW(mp::platform::vm_backend(backend_path), std::runtime_error);
}

INSTANTIATE_TEST_SUITE_P(PlatformLinux, TestUnsupportedDrivers,
                         Values(QStringLiteral("hyperkit"), QStringLiteral("hyper-v"), QStringLiteral("other")));

struct TestNetworkInterfacesInfo : public TestWithParam<std::tuple<QByteArray, std::string, mp::NetworkInterfaceInfo>>
{
};

const QByteArray ip_addr_output(
    "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000\n"
    "    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n"
    "    inet 127.0.0.1/8 scope host lo\n"
    "       valid_lft forever preferred_lft forever\n"
    "    inet6 ::1/128 scope host \n"
    "       valid_lft forever preferred_lft forever\n"
    "2: enp3s0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc fq_codel state DOWN group default qlen 1000\n"
    "    link/ether 20:47:47:fe:04:56 brd ff:ff:ff:ff:ff:ff\n"
    "3: wlx60e3270f55fe: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP group default qlen 1000\n"
    "    link/ether 60:e3:27:0f:55:fe brd ff:ff:ff:ff:ff:ff\n"
    "    inet 192.168.2.184/24 brd 192.168.2.255 scope global dynamic noprefixroute wlx60e3270f55fe\n"
    "       valid_lft 23375sec preferred_lft 23375sec\n"
    "    inet6 fd52:2ccf:f758::4ca/128 scope global noprefixroute \n"
    "       valid_lft forever preferred_lft forever\n"
    "    inet6 fdde:2681:7a2:0:3b2b:d6d8:5bf9:b45b/64 scope global noprefixroute \n"
    "       valid_lft forever preferred_lft forever\n"
    "    inet6 fd52:2ccf:f758:0:a342:79b5:e2ba:e05e/64 scope global noprefixroute \n"
    "       valid_lft forever preferred_lft forever\n"
    "    inet6 fe80::132d:7392:fe19:2ff6/64 scope link noprefixroute \n"
    "       valid_lft forever preferred_lft forever\n"
    "4: virbr0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc noqueue state DOWN group default qlen 1000\n"
    "    link/ether 52:54:00:d9:b1:0a brd ff:ff:ff:ff:ff:ff\n"
    "    inet 192.168.122.1/24 brd 192.168.122.255 scope global virbr0\n"
    "       valid_lft forever preferred_lft forever\n"
    "5: virbr0-nic: <BROADCAST,MULTICAST> mtu 1500 qdisc fq_codel master virbr0 state DOWN group default qlen 1000\n"
    "    link/ether 52:54:00:d9:b1:0a brd ff:ff:ff:ff:ff:ff\n"
    "6: mpqemubr0-dummy: <BROADCAST,NOARP> mtu 1500 qdisc noop master mpqemubr0 state DOWN group default qlen 1000\n"
    "    link/ether 52:54:00:fb:84:b4 brd ff:ff:ff:ff:ff:ff\n"
    "7: mpqemubr0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc noqueue state DOWN group default qlen 1000\n"
    "    link/ether 52:54:00:fb:84:b4 brd ff:ff:ff:ff:ff:ff\n"
    "    inet 10.248.40.1/24 brd 10.248.40.255 scope global mpqemubr0\n"
    "       valid_lft forever preferred_lft forever\n"
    "    inet6 fe80::5054:ff:fefb:84b4/64 scope link \n"
    "       valid_lft forever preferred_lft forever\n"
    "8: tap-c4218ab4e59: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc fq_codel master mpqemubr0 state DOWN group "
    "default qlen 1000\n"
    "    link/ether f6:1f:05:65:31:64 brd ff:ff:ff:ff:ff:ff\n"
    "    inet6 fe80::f41f:5ff:fe65:3164/64 scope link \n"
    "       valid_lft forever preferred_lft forever\n"
    "9: tap-6690bec4d37: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc fq_codel master mpqemubr0 state DOWN group "
    "default qlen 1000\n"
    "    link/ether 76:a9:bb:18:c5:67 brd ff:ff:ff:ff:ff:ff\n"
    "10: tun0: <POINTOPOINT,MULTICAST,NOARP,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UNKNOWN group default qlen 100\n"
    "    link/none \n"
    "    inet 10.8.8.23/24 brd 10.8.8.255 scope global noprefixroute tun0\n"
    "       valid_lft forever preferred_lft forever\n"
    "    inet6 fe80::c4be:53c0:4628:6946/64 scope link stable-privacy \n"
    "       valid_lft forever preferred_lft forever\n"
    "11: tun1: <POINTOPOINT,MULTICAST,NOARP,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UNKNOWN group default qlen 100\n"
    "    link/none \n"
    "    inet 10.172.66.58/18 brd 10.172.127.255 scope global noprefixroute tun1\n"
    "       valid_lft forever preferred_lft forever\n"
    "    inet6 2001:67c:1562:8007::aac:423a peer 2001:67c:1562:8007::1/64 scope global noprefixroute \n"
    "       valid_lft forever preferred_lft forever\n");

TEST_P(TestNetworkInterfacesInfo, test_network_interfaces_info)
{
    const auto param = GetParam();
    const auto& ip_output = std::get<0>(param);
    const auto& iface_name = std::get<1>(param);
    const auto& iface_info = std::get<2>(param);
    const mp::ProcessState ip_exit_status{0, mp::nullopt};
    auto mock_factory_scope = inject_fake_ip_callback(ip_exit_status, ip_output);

    QStringList args{"address"};
    auto interfaces_info = mp::platform::get_network_interfaces_info();

    mp::NetworkInterfaceInfo output_info = interfaces_info[iface_name];

    ASSERT_EQ(output_info.id, iface_info.id);
    ASSERT_EQ(output_info.type, iface_info.type);
    ASSERT_EQ(output_info.description, iface_info.description);
    if (iface_info.ip_address)
    {
        ASSERT_EQ(output_info.ip_address->as_string(), iface_info.ip_address->as_string());
    }
}

auto make_test_input(const std::string& id, const std::string& type, const std::string& descr, const std::string& ip)
{
    return std::make_tuple(
        ip_addr_output, id,
        mp::NetworkInterfaceInfo{id, type, descr, ip.empty() ? mp::nullopt : mp::optional<mp::IPAddress>(ip)});
}

INSTANTIATE_TEST_SUITE_P(PlatformLinux, TestNetworkInterfacesInfo,
                         Values(make_test_input("lo", "virtual", "Virtual interface", "127.0.0.1"),
                                make_test_input("enp3s0", "unknown", "Ethernet or wifi", "")));
} // namespace
