/*
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <unistd.h>
#include <srsenb/hdr/slicing_functions.h>

#include "srslte/common/log.h"
#include "srslte/common/threads.h"
#include "srslte/srslte.h"

#include "srsenb/hdr/phy/sf_worker.h"
#include "srsenb/hdr/phy/txrx.h"

#include "srsenb/hdr/global_variables.h"
#include <stdio.h>
#include <sys/file.h>

#define Error(fmt, ...)                                                                                                \
  if (SRSLTE_DEBUG_ENABLED)                                                                                            \
  log_h->error(fmt, ##__VA_ARGS__)
#define Warning(fmt, ...)                                                                                              \
  if (SRSLTE_DEBUG_ENABLED)                                                                                            \
  log_h->warning(fmt, ##__VA_ARGS__)
#define Info(fmt, ...)                                                                                                 \
  if (SRSLTE_DEBUG_ENABLED)                                                                                            \
  log_h->info(fmt, ##__VA_ARGS__)
#define Debug(fmt, ...)                                                                                                \
  if (SRSLTE_DEBUG_ENABLED)                                                                                            \
  log_h->debug(fmt, ##__VA_ARGS__)

using namespace std;

namespace srsenb {

txrx::txrx() : thread("TXRX")
{
  /* Do nothing */
}

bool txrx::init(srslte::radio_interface_phy* radio_h_,
                srslte::thread_pool*         workers_pool_,
                phy_common*                  worker_com_,
                prach_worker_pool*           prach_,
                srslte::log*                 log_h_,
                uint32_t                     prio_)
{
  radio_h       = radio_h_;
  log_h         = log_h_;
  workers_pool  = workers_pool_;
  worker_com    = worker_com_;
  prach         = prach_;
  tx_worker_cnt = 0;
  running       = true;

  nof_workers = workers_pool->get_nof_workers();

  // Instantiate UL channel emulator
  if (worker_com->params.ul_channel_args.enable) {
    ul_channel = srslte::channel_ptr(new srslte::channel(worker_com->params.ul_channel_args, 1));
  }

  start(prio_);
  return true;
}

void txrx::stop()
{
  if (running) {
    running = false;
    wait_thread_finish();
  }
}

void txrx::run_thread()
{
  sf_worker*          worker  = nullptr;
  srslte::rf_buffer_t buffer  = {};
  srslte_timestamp_t  rx_time = {};
  srslte_timestamp_t  tx_time = {};
  uint32_t            sf_len  = SRSLTE_SF_LEN_PRB(worker_com->get_nof_prb(0));

  float samp_rate = srslte_sampling_freq_hz(worker_com->get_nof_prb(0));

    std::string global_config_dir_path = SCOPE_CONFIG_DIR;
    std::string gloabl_config_file_name = "scope_cfg.txt";

    // SCOPE: read and print network slicing parameter from configuration file
    network_slicing_enabled = (int) read_config_parameter(global_config_dir_path, gloabl_config_file_name,
                                                          "network_slicing_enabled");

  // set to 0 if configuration file is not found
  if (network_slicing_enabled == -1)
      network_slicing_enabled = 0;

  if (network_slicing_enabled) {
      log_h->console("\nNetwork slicing enabled with up to %d tenants\n", MAX_SLICING_TENANTS);
  }
  else {
      log_h->console("\nNetwork slicing disabled\n");
  }

  // SCOPE: get and print scheduler configuration info
  global_scheduling_policy = (int) read_config_parameter(global_config_dir_path, gloabl_config_file_name,
                                                         "global_scheduling_policy");

  // set to 1 (waterfilling) if configuration file is not found
  if (global_scheduling_policy == -1)
      global_scheduling_policy = 1;

  if (global_scheduling_policy == 0) {
      log_h->console("\nGlobal scheduler: Round-robin\n");
  }
  else {
      log_h->console("\nGlobal scheduler: Waterfilling\n");
  }

  // SCOPE: read parameter to force downlink/uplink modulation
  force_dl_modulation = (int) read_config_parameter(global_config_dir_path, gloabl_config_file_name, "force_dl_modulation");
  force_ul_modulation = (int) read_config_parameter(global_config_dir_path, gloabl_config_file_name, "force_ul_modulation");

  // disable if not found
  if (force_dl_modulation == -1) {
      force_dl_modulation = 0;
  }

  if (force_dl_modulation == 1) {
      log_h->console("\nForcing downlink modulation\n");
  }

  if (force_ul_modulation == -1) {
      force_ul_modulation = 0;
  }

  if (force_ul_modulation == 1) {
      log_h->console("\nForcing uplink modulation\n");
  }

  // SCOPE: read in srsenb/src/enb_cfg_parser.cc but printed here for convenience 
  if (colosseum_testbed)
      log_h->console("\nRunning on Colosseum\n");

  // Configure radio
  radio_h->set_rx_srate(samp_rate);
  radio_h->set_tx_srate(samp_rate);

  // Set Tx/Rx frequencies
  for (uint32_t cc_idx = 0; cc_idx < worker_com->get_nof_carriers(); cc_idx++) {
    double   tx_freq_hz = worker_com->get_dl_freq_hz(cc_idx);
    double   rx_freq_hz = worker_com->get_ul_freq_hz(cc_idx);
    uint32_t rf_port    = worker_com->get_rf_port(cc_idx);
    log_h->console(
        "Setting frequency: DL=%.1f Mhz, UL=%.1f MHz for cc_idx=%d\n", tx_freq_hz / 1e6f, rx_freq_hz / 1e6f, cc_idx);
    radio_h->set_tx_freq(rf_port, tx_freq_hz);
    radio_h->set_rx_freq(rf_port, rx_freq_hz);
  }

  // Set channel emulator sampling rate
  if (ul_channel) {
    ul_channel->set_srate(static_cast<uint32_t>(samp_rate));
  }

  log_h->info("Starting RX/TX thread nof_prb=%d, sf_len=%d\n", worker_com->get_nof_prb(0), sf_len);

  // Set TTI so that first TX is at tti=0
  tti = TTI_SUB(0, FDD_HARQ_DELAY_UL_MS + 1);

  // SCOPE DL TX gain config filename
  std::string dl_tx_gain_config_file = SCOPE_CONFIG_DIR;
  dl_tx_gain_config_file += "slicing/tx_gain_dl.txt";

  // Main loop
  while (running) {
    tti    = TTI_ADD(tti, 1);
    worker = (sf_worker*)workers_pool->wait_worker(tti);
    if (worker) {
      // Multiple cell buffer mapping
      for (uint32_t cc = 0; cc < worker_com->get_nof_carriers(); cc++) {
        uint32_t rf_port = worker_com->get_rf_port(cc);
        for (uint32_t p = 0; p < worker_com->get_nof_ports(cc); p++) {
          // WARNING: The number of ports for all cells must be the same
          buffer.set(rf_port, p, worker_com->get_nof_ports(0), worker->get_buffer_rx(cc, p));
        }
      }

      radio_h->rx_now(buffer, sf_len, &rx_time);

      if (last_time_gain_ctrl_counter % 250 == 0) {
          config_file = fopen(dl_tx_gain_config_file.c_str(), "r");

          if (config_file) {

              // lock file and check flock return value
              int ret_fl = flock(fileno(config_file), LOCK_EX);
              if (ret_fl == -1) {
                fclose(config_file);
                log_h->info("DL TX gain configuration: flock return value is -1 (%s)!\n",
                            dl_tx_gain_config_file.c_str());
                break;
              }

              if (fgets(tx_gain_ctrl_str, sizeof(tx_gain_ctrl_str), config_file) != NULL) {
                float gain = strtof(tx_gain_ctrl_str, NULL);
                log_h->info("DL TX gain configuration: set gain to (%.1f)\n", gain);
                radio_h->set_tx_gain(gain);
              }
              else {
                log_h->info("DL TX gain configuration: unknown control string (%s)\n", tx_gain_ctrl_str);
                break;
              }
              // remove is too slow and will crash srsRAN, so don't do this!
              //remove("agent_cmd.bin");  // don't allow srsRAN to reread a command
          } else {
              log_h->info("DL TX gain configuration: file not found (%s)!\n", dl_tx_gain_config_file.c_str());
          }
          fclose(config_file);
      }

      last_time_gain_ctrl_counter = (last_time_gain_ctrl_counter + 1) % 1000;



      if (ul_channel) {
        ul_channel->run(buffer.to_cf_t(), buffer.to_cf_t(), sf_len, rx_time);
      }

      /* Compute TX time: Any transmission happens in TTI+4 thus advance 4 ms the reception time */
      srslte_timestamp_copy(&tx_time, &rx_time);
      srslte_timestamp_add(&tx_time, 0, FDD_HARQ_DELAY_UL_MS * 1e-3);

      Debug("Setting TTI=%d, tx_mutex=%d, tx_time=%ld:%f to worker %d\n",
            tti,
            tx_worker_cnt,
            tx_time.full_secs,
            tx_time.frac_secs,
            worker->get_id());

      worker->set_time(tti, tx_worker_cnt, tx_time);
      tx_worker_cnt = (tx_worker_cnt + 1) % nof_workers;

      // Trigger phy worker execution
      worker_com->semaphore.push(worker);
      workers_pool->start_worker(worker);

      // Trigger prach worker execution
      for (uint32_t cc = 0; cc < worker_com->get_nof_carriers(); cc++) {
        prach->new_tti(cc, tti, buffer.get(worker_com->get_rf_port(cc), 0, worker_com->get_nof_ports(0)));
      }
    } else {
      // wait_worker() only returns NULL if it's being closed. Quit now to avoid unnecessary loops here
      running = false;
    }
  }
}

} // namespace srsenb
