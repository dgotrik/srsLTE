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

#define Error(fmt, ...) log_h->error(fmt, ##__VA_ARGS__)
#define Warning(fmt, ...) log_h->warning(fmt, ##__VA_ARGS__)
#define Info(fmt, ...) log_h->info(fmt, ##__VA_ARGS__)
#define Debug(fmt, ...) log_h->debug(fmt, ##__VA_ARGS__)

#include "srsue/hdr/stack/mac/mac_nr.h"

using namespace asn1::rrc;

namespace srsue {

mac_nr::mac_nr() : pool(srslte::byte_buffer_pool::get_instance()), log_h("MAC")
{
  tx_buffer  = srslte::allocate_unique_buffer(*pool);
  rlc_buffer = srslte::allocate_unique_buffer(*pool);

  log_h->set_level(args.log_level);
  log_h->set_hex_limit(args.log_hex_limit);
}

mac_nr::~mac_nr()
{
  stop();
}

int mac_nr::init(const mac_nr_args_t&            args_,
                 phy_interface_mac_nr*           phy_,
                 rlc_interface_mac*              rlc_,
                 srslte::timer_handler*          timers_,
                 srslte::task_handler_interface* stack_)
{
  args   = args_;
  phy    = phy_;
  rlc    = rlc_;
  timers = timers_;
  stack  = stack_;

  // Create Stack task dispatch queue
  stack_task_dispatch_queue = stack->make_task_queue();

  // Set up pcap
  if (args.pcap.enable) {
    pcap.reset(new srslte::mac_nr_pcap());
    pcap->open(args.pcap.filename);
  }

  started = true;

  return SRSLTE_SUCCESS;
}

void mac_nr::stop()
{
  if (started) {
    if (pcap != nullptr) {
      pcap->close();
    }

    started = false;
  }
}

// Implement Section 5.9
void mac_nr::reset()
{
  Info("Resetting MAC\n");
}

void mac_nr::run_tti(const uint32_t tti)
{
  log_h->step(tti);

  // Step all procedures
  Debug("Running MAC tti=%d\n", tti);
}

uint16_t mac_nr::get_ul_sched_rnti(uint32_t tti)
{
  return crnti;
}

uint16_t mac_nr::get_dl_sched_rnti(uint32_t tti)
{
  return crnti;
}

void mac_nr::bch_decoded_ok(uint32_t tti, srslte::unique_byte_buffer_t payload)
{
  // Send MIB to RLC
  rlc->write_pdu_bcch_bch(std::move(payload));

  if (pcap) {
    // pcap->write_dl_bch(payload, len, true, tti);
  }
}

int mac_nr::sf_indication(const uint32_t tti)
{

  return SRSLTE_SUCCESS;
}

/**
 * \brief Called from PHY after decoding a TB
 *
 * The TB can directly be used
 *
 * @param cc_idx
 * @param grant structure
 */
void mac_nr::tb_decoded(const uint32_t cc_idx, mac_nr_grant_dl_t& grant)
{
  if (SRSLTE_RNTI_ISRAR(grant.rnti)) {
    // TODO: deliver to RA procedure
  } else if (grant.rnti == SRSLTE_PRNTI) {
    // Send PCH payload to RLC
    // rlc->write_pdu_pcch(pch_payload_buffer, grant.tb[0].tbs);

    if (pcap) {
      // pcap->write_dl_pch(pch_payload_buffer, grant.tb[0].tbs, true, grant.tti);
    }
  } else {
    for (uint32_t i = 0; i < SRSLTE_MAX_CODEWORDS; ++i) {
      if (grant.tb[i] != nullptr) {
        if (pcap) {
          pcap->write_dl_crnti(grant.tb[i]->msg, grant.tb[i]->N_bytes, grant.rnti, true, grant.tti);
        }
        pdu_queue.push(std::move(grant.tb[i]));

        metrics[cc_idx].rx_pkts++;
      }
    }

    process_pdus();
  }
}

void mac_nr::new_grant_ul(const uint32_t cc_idx, const mac_nr_grant_ul_t& grant)
{
  phy_interface_stack_nr::tx_request_t tx_request = {};

  get_ul_data(grant, &tx_request);

  // send TX.request
  phy->tx_request(tx_request);

  metrics[cc_idx].tx_pkts++;
}

void mac_nr::get_ul_data(const mac_nr_grant_ul_t& grant, phy_interface_stack_nr::tx_request_t* tx_request)
{
  // Todo: delegate to mux class
  tx_request->tb_len = grant.tbs;

  // initialize MAC PDU
  tx_buffer->clear();
  tx_pdu.init_tx(tx_buffer.get(), grant.tbs, true);

  while (tx_pdu.get_remaing_len() >= MIN_RLC_PDU_LEN) {
    // read RLC PDU
    rlc_buffer->clear();
    int pdu_len = rlc->read_pdu(4, rlc_buffer->msg, tx_pdu.get_remaing_len() - 2);

    // Add SDU if RLC has something to tx
    if (pdu_len > 0) {
      rlc_buffer->N_bytes = pdu_len;
      log_h->info_hex(rlc_buffer->msg, rlc_buffer->N_bytes, "Read %d B from RLC\n", rlc_buffer->N_bytes);

      // add to MAC PDU and pack
      if (tx_pdu.add_sdu(4, rlc_buffer->msg, rlc_buffer->N_bytes) != SRSLTE_SUCCESS) {
        log_h->error("Error packing MAC PDU\n");
      }
    }
  }

  // Pack PDU
  tx_pdu.pack();

  log_h->info_hex(tx_buffer->msg, tx_buffer->N_bytes, "Generated MAC PDU (%d B)\n", tx_buffer->N_bytes);

  tx_request->data   = tx_buffer->msg;
  tx_request->tb_len = tx_buffer->N_bytes;

  if (pcap) {
    pcap->write_ul_crnti(tx_request->data, tx_request->tb_len, grant.rnti, grant.pid, grant.tti);
  }
}

void mac_nr::timer_expired(uint32_t timer_id)
{
  // not implemented
}

void mac_nr::setup_lcid(uint32_t lcid, uint32_t lcg, uint32_t priority, int PBR_x_tti, uint32_t BSD)
{
  logical_channel_config_t config = {};
  config.lcid                     = lcid;
  config.lcg                      = lcg;
  config.priority                 = priority;
  config.PBR                      = PBR_x_tti;
  config.BSD                      = BSD;
  config.bucket_size              = config.PBR * config.BSD;
  setup_lcid(config);
}

void mac_nr::setup_lcid(const logical_channel_config_t& config)
{
  Info("Logical Channel Setup: LCID=%d, LCG=%d, priority=%d, PBR=%d, BSD=%dms, bucket_size=%d\n",
       config.lcid,
       config.lcg,
       config.priority,
       config.PBR,
       config.BSD,
       config.bucket_size);
  // mux_unit.setup_lcid(config);
  // bsr_procedure.setup_lcid(config.lcid, config.lcg, config.priority);
}

void mac_nr::get_metrics(mac_metrics_t m[SRSLTE_MAX_CARRIERS]) {}

/**
 * Called from the main stack thread to process received PDUs
 */
void mac_nr::process_pdus()
{
  auto ret = stack_task_dispatch_queue.try_push([this]() {
    while (started and not pdu_queue.empty()) {
      srslte::unique_byte_buffer_t pdu = pdu_queue.wait_pop();
      // TODO: delegate to demux class
      handle_pdu(std::move(pdu));
    }
  });
}

void mac_nr::handle_pdu(srslte::unique_byte_buffer_t pdu)
{
  log_h->info_hex(pdu->msg, pdu->N_bytes, "Handling MAC PDU (%d B)\n", pdu->N_bytes);
}

} // namespace srsue