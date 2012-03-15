/******************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2006-2008  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  ---
 *
 *  The license mentioned above concerns the source code only. Using the
 *  EtherCAT technology and brand is only permitted in compliance with the
 *  industrial property and similar rights of Beckhoff Automation GmbH.
 *
 *****************************************************************************/

/**
   \file
   EtherCAT domain methods.
*/

/*****************************************************************************/

#include <linux/module.h>

#include "globals.h"
#include "master.h"
#include "slave_config.h"

#include "domain.h"
#include "datagram_pair.h"

/*****************************************************************************/

void ec_domain_clear_data(ec_domain_t *);

/*****************************************************************************/

/** Domain constructor.
 */
void ec_domain_init(
        ec_domain_t *domain, /**< EtherCAT domain. */
        ec_master_t *master, /**< Parent master. */
        unsigned int index /**< Index. */
        )
{
    domain->master = master;
    domain->index = index;
    INIT_LIST_HEAD(&domain->fmmu_configs);
    domain->data_size = 0;
    domain->data = NULL;
    domain->data_origin = EC_ORIG_INTERNAL;
    domain->logical_base_address = 0x00000000;
    INIT_LIST_HEAD(&domain->datagram_pairs);
    domain->working_counter = 0x0000;
    domain->expected_working_counter = 0x0000;
    domain->working_counter_changes = 0;
    domain->notify_jiffies = 0;
}

/*****************************************************************************/

/** Domain destructor.
 */
void ec_domain_clear(ec_domain_t *domain /**< EtherCAT domain */)
{
    ec_datagram_pair_t *datagram_pair, *next_pair;

    // dequeue and free datagrams
    list_for_each_entry_safe(datagram_pair, next_pair,
            &domain->datagram_pairs, list) {
        ec_datagram_pair_clear(datagram_pair);
        kfree(datagram_pair);
    }

    ec_domain_clear_data(domain);
}

/*****************************************************************************/

/** Frees internally allocated memory.
 */
void ec_domain_clear_data(
        ec_domain_t *domain /**< EtherCAT domain. */
        )
{
    if (domain->data_origin == EC_ORIG_INTERNAL && domain->data) {
        kfree(domain->data);
    }

    domain->data = NULL;
    domain->data_origin = EC_ORIG_INTERNAL;
}

/*****************************************************************************/

/** Adds an FMMU configuration to the domain.
 */
void ec_domain_add_fmmu_config(
        ec_domain_t *domain, /**< EtherCAT domain. */
        ec_fmmu_config_t *fmmu /**< FMMU configuration. */
        )
{
    fmmu->domain = domain;

    domain->data_size += fmmu->data_size;
    list_add_tail(&fmmu->list, &domain->fmmu_configs);

    EC_MASTER_DBG(domain->master, 1, "Domain %u:"
            " Added %u bytes, total %zu.\n",
            domain->index, fmmu->data_size, domain->data_size);
}

/*****************************************************************************/

/** Allocates a domain datagram pair and appends it to the list.
 *
 * The datagrams' types and expected working counters are determined by the
 * number of input and output fmmus that share the datagrams.
 *
 * \retval  0 Success.
 * \retval <0 Error code.
 */
int ec_domain_add_datagram_pair(
        ec_domain_t *domain, /**< EtherCAT domain. */
        uint32_t logical_offset, /**< Logical offset. */
        size_t data_size, /**< Size of the data. */
        uint8_t *data, /**< Process data. */
        const unsigned int used[] /**< Used by inputs/outputs. */
        )
{
    ec_datagram_pair_t *datagram_pair;
    int ret;
    unsigned int dev_idx;

    if (!(datagram_pair = kmalloc(sizeof(ec_datagram_pair_t), GFP_KERNEL))) {
        EC_MASTER_ERR(domain->master,
                "Failed to allocate domain datagram pair!\n");
        return -ENOMEM;
    }

    ec_datagram_pair_init(datagram_pair);

    /* backup datagram has its own memory */
    ret = ec_datagram_prealloc(&datagram_pair->datagrams[EC_DEVICE_BACKUP],
            data_size);
    if (ret) {
        ec_datagram_pair_clear(datagram_pair);
        kfree(datagram_pair);
        return ret;
    }

    /* The ec_datagram_lxx() calls below can not fail, because either the
     * datagram has external memory or it is preallocated. */

    if (used[EC_DIR_OUTPUT] && used[EC_DIR_INPUT]) { // inputs and outputs
        ec_datagram_lrw_ext(&datagram_pair->datagrams[EC_DEVICE_MAIN],
                logical_offset, data_size, data);
        ec_datagram_lrw(&datagram_pair->datagrams[EC_DEVICE_BACKUP],
                logical_offset, data_size);

        // If LRW is used, output FMMUs increment the working counter by 2,
        // while input FMMUs increment it by 1.
        domain->expected_working_counter +=
            used[EC_DIR_OUTPUT] * 2 + used[EC_DIR_INPUT];
    } else if (used[EC_DIR_OUTPUT]) { // outputs only
        ec_datagram_lwr_ext(&datagram_pair->datagrams[EC_DEVICE_MAIN],
                logical_offset, data_size, data);
        ec_datagram_lwr(&datagram_pair->datagrams[EC_DEVICE_BACKUP],
                logical_offset, data_size);

        domain->expected_working_counter += used[EC_DIR_OUTPUT];
    } else { // inputs only (or nothing)
        ec_datagram_lrd_ext(&datagram_pair->datagrams[EC_DEVICE_MAIN],
                logical_offset, data_size, data);
        ec_datagram_lrd(&datagram_pair->datagrams[EC_DEVICE_BACKUP],
                logical_offset, data_size);

        domain->expected_working_counter += used[EC_DIR_INPUT];
    }

    for (dev_idx = 0; dev_idx < EC_NUM_DEVICES; dev_idx++) {
        snprintf(datagram_pair->datagrams[dev_idx].name,
                EC_DATAGRAM_NAME_SIZE, "domain%u-%u-%s", domain->index,
                logical_offset, dev_idx ? "backup" : "main");
        ec_datagram_zero(&datagram_pair->datagrams[dev_idx]);
    }

    list_add_tail(&datagram_pair->list, &domain->datagram_pairs);
    return 0;
}

/*****************************************************************************/

/** Finishes a domain.
 *
 * This allocates the necessary datagrams and writes the correct logical
 * addresses to every configured FMMU.
 *
 * \todo Check for FMMUs that do not fit into any datagram.
 *
 * \retval  0 Success
 * \retval <0 Error code.
 */
int ec_domain_finish(
        ec_domain_t *domain, /**< EtherCAT domain. */
        uint32_t base_address /**< Logical base address. */
        )
{
    uint32_t datagram_offset;
    size_t datagram_size;
    unsigned int datagram_count;
    unsigned int datagram_used[EC_DIR_COUNT];
    ec_fmmu_config_t *fmmu;
    ec_fmmu_config_t *fmmu_temp;
    const ec_datagram_pair_t *datagram_pair;
    int ret;

    domain->logical_base_address = base_address;

    if (domain->data_size && domain->data_origin == EC_ORIG_INTERNAL) {
        if (!(domain->data =
                    (uint8_t *) kmalloc(domain->data_size, GFP_KERNEL))) {
            EC_MASTER_ERR(domain->master, "Failed to allocate %zu bytes"
                    " internal memory for domain %u!\n",
                    domain->data_size, domain->index);
            return -ENOMEM;
        }
    }

    // Cycle through all domain FMMUS and
    // - correct the logical base addresses
    // - set up the datagrams to carry the process data
    datagram_offset = 0;
    datagram_size = 0;
    datagram_count = 0;
    datagram_used[EC_DIR_OUTPUT] = 0;
    datagram_used[EC_DIR_INPUT] = 0;

    list_for_each_entry(fmmu_temp, &domain->fmmu_configs, list) {
        // we have to remove the constness, sorry FIXME
        ec_slave_config_t *sc = (ec_slave_config_t *) fmmu_temp->sc;
        sc->used_for_fmmu_datagram[fmmu_temp->dir] = 0;
    }

    list_for_each_entry(fmmu, &domain->fmmu_configs, list) {
        // Correct logical FMMU address
        fmmu->logical_start_address += base_address;

        // Increment Input/Output counter to determine datagram types
        // and calculate expected working counters
        if (fmmu->sc->used_for_fmmu_datagram[fmmu->dir] == 0) {
            ec_slave_config_t *sc = (ec_slave_config_t *)fmmu->sc;
            datagram_used[fmmu->dir]++;
            sc->used_for_fmmu_datagram[fmmu->dir] = 1;
        }

        // If the current FMMU's data do not fit in the current datagram,
        // allocate a new one.
        if (datagram_size + fmmu->data_size > EC_MAX_DATA_SIZE) {
            ret = ec_domain_add_datagram_pair(domain,
                    domain->logical_base_address + datagram_offset,
                    datagram_size, domain->data + datagram_offset,
                    datagram_used);
            if (ret < 0)
                return ret;
            datagram_offset += datagram_size;
            datagram_size = 0;
            datagram_count++;
            datagram_used[EC_DIR_OUTPUT] = 0;
            datagram_used[EC_DIR_INPUT] = 0;
            list_for_each_entry(fmmu_temp, &domain->fmmu_configs, list) {
                ec_slave_config_t *sc = (ec_slave_config_t *)fmmu_temp->sc;
               sc->used_for_fmmu_datagram[fmmu_temp->dir] = 0;
            }
        }

        datagram_size += fmmu->data_size;
    }

    /* Allocate last datagram pair, if data are left (this is also the case if
     * the process data fit into a single datagram) */
    if (datagram_size) {
        ret = ec_domain_add_datagram_pair(domain,
                domain->logical_base_address + datagram_offset,
                datagram_size, domain->data + datagram_offset,
                datagram_used);
        if (ret < 0)
            return ret;
        datagram_count++;
    }

    EC_MASTER_INFO(domain->master, "Domain%u: Logical address 0x%08x,"
            " %zu byte, expected working counter %u.\n", domain->index,
            domain->logical_base_address, domain->data_size,
            domain->expected_working_counter);

    list_for_each_entry(datagram_pair, &domain->datagram_pairs, list) {
        const ec_datagram_t *datagram =
            &datagram_pair->datagrams[EC_DEVICE_MAIN];
        EC_MASTER_INFO(domain->master, "  Datagram %s: Logical offset 0x%08x,"
                " %zu byte, type %s.\n", datagram->name,
                EC_READ_U32(datagram->address), datagram->data_size,
                ec_datagram_type_string(datagram));
    }

    return 0;
}

/*****************************************************************************/

/** Get the number of FMMU configurations of the domain.
 */
unsigned int ec_domain_fmmu_count(const ec_domain_t *domain)
{
    const ec_fmmu_config_t *fmmu;
    unsigned int num = 0;

    list_for_each_entry(fmmu, &domain->fmmu_configs, list) {
        num++;
    }

    return num;
}

/*****************************************************************************/

/** Get a certain FMMU configuration via its position in the list.
 */
const ec_fmmu_config_t *ec_domain_find_fmmu(
        const ec_domain_t *domain, /**< EtherCAT domain. */
        unsigned int pos /**< List position. */
        )
{
    const ec_fmmu_config_t *fmmu;

    list_for_each_entry(fmmu, &domain->fmmu_configs, list) {
        if (pos--)
            continue;
        return fmmu;
    }

    return NULL;
}

/******************************************************************************
 *  Application interface
 *****************************************************************************/

int ecrt_domain_reg_pdo_entry_list(ec_domain_t *domain,
        const ec_pdo_entry_reg_t *regs)
{
    const ec_pdo_entry_reg_t *reg;
    ec_slave_config_t *sc;
    int ret;

    EC_MASTER_DBG(domain->master, 1, "ecrt_domain_reg_pdo_entry_list("
            "domain = 0x%p, regs = 0x%p)\n", domain, regs);

    for (reg = regs; reg->index; reg++) {
        sc = ecrt_master_slave_config_err(domain->master, reg->alias,
                reg->position, reg->vendor_id, reg->product_code);
        if (IS_ERR(sc))
            return PTR_ERR(sc);

        ret = ecrt_slave_config_reg_pdo_entry(sc, reg->index,
                        reg->subindex, domain, reg->bit_position);
        if (ret < 0)
            return ret;

        *reg->offset = ret;
    }

    return 0;
}

/*****************************************************************************/

size_t ecrt_domain_size(const ec_domain_t *domain)
{
    return domain->data_size;
}

/*****************************************************************************/

void ecrt_domain_external_memory(ec_domain_t *domain, uint8_t *mem)
{
    EC_MASTER_DBG(domain->master, 1, "ecrt_domain_external_memory("
            "domain = 0x%p, mem = 0x%p)\n", domain, mem);

    down(&domain->master->master_sem);

    ec_domain_clear_data(domain);

    domain->data = mem;
    domain->data_origin = EC_ORIG_EXTERNAL;

    up(&domain->master->master_sem);
}

/*****************************************************************************/

uint8_t *ecrt_domain_data(ec_domain_t *domain)
{
    return domain->data;
}

/*****************************************************************************/

void ecrt_domain_process(ec_domain_t *domain)
{
    uint16_t working_counter_sum;
    ec_datagram_pair_t *datagram_pair;
    unsigned int dev_idx;

    working_counter_sum = 0;
    list_for_each_entry(datagram_pair, &domain->datagram_pairs, list) {
        for (dev_idx = 0; dev_idx < EC_NUM_DEVICES; dev_idx++) {
            ec_datagram_t *datagram = &datagram_pair->datagrams[dev_idx];
            ec_datagram_output_stats(datagram);
            if (datagram->state == EC_DATAGRAM_RECEIVED) {
                working_counter_sum += datagram->working_counter;
            }
        }
    }

    if (working_counter_sum != domain->working_counter) {
        domain->working_counter_changes++;
        domain->working_counter = working_counter_sum;
    }

    if (domain->working_counter_changes &&
        jiffies - domain->notify_jiffies > HZ) {
        domain->notify_jiffies = jiffies;
        if (domain->working_counter_changes == 1) {
            EC_MASTER_INFO(domain->master, "Domain %u: Working counter"
                    " changed to %u/%u.\n", domain->index,
                    domain->working_counter,
                    domain->expected_working_counter);
        } else {
            EC_MASTER_INFO(domain->master, "Domain %u: %u working counter"
                    " changes - now %u/%u.\n", domain->index,
                    domain->working_counter_changes, domain->working_counter,
                    domain->expected_working_counter);
        }
        domain->working_counter_changes = 0;
    }
}

/*****************************************************************************/

void ecrt_domain_queue(ec_domain_t *domain)
{
    ec_datagram_pair_t *datagram_pair;
    unsigned int dev_idx;

    list_for_each_entry(datagram_pair, &domain->datagram_pairs, list) {

        /* copy main data to backup datagram */
        memcpy(datagram_pair->datagrams[EC_DEVICE_BACKUP].data,
                datagram_pair->datagrams[EC_DEVICE_MAIN].data,
                datagram_pair->datagrams[EC_DEVICE_MAIN].data_size);

        for (dev_idx = 0; dev_idx < EC_NUM_DEVICES; dev_idx++) {
            ec_master_queue_datagram(domain->master,
                    &datagram_pair->datagrams[dev_idx], dev_idx);
        }
    }
}

/*****************************************************************************/

void ecrt_domain_state(const ec_domain_t *domain, ec_domain_state_t *state)
{
    state->working_counter = domain->working_counter;

    if (domain->working_counter) {
        if (domain->working_counter == domain->expected_working_counter) {
            state->wc_state = EC_WC_COMPLETE;
        } else {
            state->wc_state = EC_WC_INCOMPLETE;
        }
    } else {
        state->wc_state = EC_WC_ZERO;
    }
}

/*****************************************************************************/

/** \cond */

EXPORT_SYMBOL(ecrt_domain_reg_pdo_entry_list);
EXPORT_SYMBOL(ecrt_domain_size);
EXPORT_SYMBOL(ecrt_domain_external_memory);
EXPORT_SYMBOL(ecrt_domain_data);
EXPORT_SYMBOL(ecrt_domain_process);
EXPORT_SYMBOL(ecrt_domain_queue);
EXPORT_SYMBOL(ecrt_domain_state);

/** \endcond */

/*****************************************************************************/
