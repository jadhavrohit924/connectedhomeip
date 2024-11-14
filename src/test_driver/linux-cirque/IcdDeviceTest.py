#!/usr/bin/env python3
"""
Copyright (c) 2021 Project CHIP Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import logging
import os
import sys

from helper.CHIPTestBase import CHIPVirtualHome

logger = logging.getLogger('MobileDeviceTest')
logger.setLevel(logging.INFO)

sh = logging.StreamHandler()
sh.setFormatter(
    logging.Formatter(
        '%(asctime)s [%(name)s] %(levelname)s %(message)s'))
logger.addHandler(sh)

CHIP_PORT = 5540

CIRQUE_URL = "http://localhost:5000"
CHIP_REPO = os.path.join(os.path.abspath(
    os.path.dirname(__file__)), "..", "..", "..")
TEST_EXTPANID = "fedcba9876543210"
TEST_DISCRIMINATOR = 3840
TEST_DISCRIMINATOR2 = 3584
TEST_DISCRIMINATOR3 = 1203
TEST_DISCRIMINATOR4 = 2145
TEST_DISCOVERY_TYPE = [0, 1, 2]
MATTER_DEVELOPMENT_PAA_ROOT_CERTS = "credentials/development/paa-root-certs"

TEST_EVENT_KEY_HEX = "00112233445566778899aabbccddeeff"

DEVICE_CONFIG = {
    'device0': {
        'type': 'MobileDevice',
        'base_image': '@default',
        'capability': ['TrafficControl', 'Mount'],
        'rcp_mode': True,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 25},
        "mount_pairs": [[CHIP_REPO, CHIP_REPO]],
    },
    'device1': {
        'type': 'CHIPEndDevice',
        'base_image': '@default',
        'capability': ['Thread', 'TrafficControl', 'Mount'],
        'rcp_mode': True,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 25},
        "mount_pairs": [[CHIP_REPO, CHIP_REPO]],
    }
}


class TestCommissioner(CHIPVirtualHome):
    def __init__(self, device_config):
        super().__init__(CIRQUE_URL, device_config)
        self.logger = logger

    def setup(self):
        self.initialize_home()

    def test_routine(self):
        self.run_controller_test()

    def run_controller_test(self):
        ethernet_ip = [device['description']['ipv6_addr'] for device in self.non_ap_devices
                       if device['type'] == 'CHIPEndDevice'][0]
        server_ids = [device['id'] for device in self.non_ap_devices
                      if device['type'] == 'CHIPEndDevice']
        req_ids = [device['id'] for device in self.non_ap_devices
                   if device['type'] == 'MobileDevice']

        for server in server_ids:
            self.execute_device_cmd(
                server,
                ("CHIPCirqueDaemon.py -- run gdb -batch -return-child-result -q -ex \"set pagination off\" "
                 "-ex run -ex \"thread apply all bt\" --args {} --thread --discriminator {} --enable-key {}").format(
                    os.path.join(CHIP_REPO, "out/debug/lit_icd/lit-icd-app"), TEST_DISCRIMINATOR, TEST_EVENT_KEY_HEX))

        self.reset_thread_devices(server_ids)

        req_device_id = req_ids[0]

        self.execute_device_cmd(req_device_id, "pip3 install --break-system-packages {}".format(os.path.join(
            CHIP_REPO, "out/debug/linux_x64_gcc/controller/python/chip_clusters-0.0-py3-none-any.whl")))
        self.execute_device_cmd(req_device_id, "pip3 install --break-system-packages {}".format(os.path.join(
            CHIP_REPO, "out/debug/linux_x64_gcc/controller/python/chip_core-0.0-cp37-abi3-linux_x86_64.whl")))
        self.execute_device_cmd(req_device_id, "pip3 install --break-system-packages {}".format(os.path.join(
            CHIP_REPO, "out/debug/linux_x64_gcc/controller/python/chip_repl-0.0-py3-none-any.whl")))

        command = ("gdb -batch -return-child-result -q -ex run -ex \"thread apply all bt\" "
                   "--args python3 {} -t 300 -a {} --paa-trust-store-path {} --test-event-key {}").format(
            os.path.join(
                CHIP_REPO, "src/controller/python/test/test_scripts/icd_device_test.py"), ethernet_ip,
            os.path.join(CHIP_REPO, MATTER_DEVELOPMENT_PAA_ROOT_CERTS), TEST_EVENT_KEY_HEX)
        ret = self.execute_device_cmd(req_device_id, command)

        self.assertEqual(ret['return_code'], '0',
                         "Test failed: non-zero return code")


if __name__ == "__main__":
    sys.exit(TestCommissioner(DEVICE_CONFIG).run_test())