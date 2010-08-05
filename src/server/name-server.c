#include "name-server.h"
#include "zone-node.h"
#include "dns-simple.h"
#include "zone-database.h"
#include <stdio.h>
#include <assert.h>

#include <urcu.h>
#include <ldns/dname.h>
#include <ldns/rdata.h>

//#define NS_DEBUG

/*----------------------------------------------------------------------------*/

ns_nameserver *ns_create( zdb_database *database )
{
    ns_nameserver *ns = malloc(sizeof(ns_nameserver));
    if (ns == NULL) {
        ERR_ALLOC_FAILED;
        return NULL;
    }
    ns->zone_db = database;
    return ns;
}

/*----------------------------------------------------------------------------*/

int ns_answer_request( ns_nameserver *nameserver, const char *query_wire,
                       uint qsize, char *response_wire, uint *rsize )
{
    debug_ns("ns_answer_request() called with query size %d.\n", qsize);
    debug_ns_hex(query_wire, qsize);

    dnss_packet *query = dnss_parse_query(query_wire, qsize);
    if (query == NULL || query->header.qdcount <= 0) {
        return -1;
    }

    debug_ns("Query parsed, ID: %u, QNAME: %s\n", query->header.id,
           query->questions[0].qname);
    debug_ns_hex(query->questions[0].qname, strlen(query->questions[0].qname));

	// start of RCU read critical section (getting the node from the database)
	rcu_read_lock();

	ldns_rdf *qname_ldns = ldns_dname_new_frm_data(
			strlen(query->questions[0].qname) + 1, query->questions[0].qname);

    const zn_node *node =
			zdb_find_name(nameserver->zone_db, qname_ldns);

    dnss_packet *response = dnss_create_empty_packet();
    if (response == NULL) {
        dnss_destroy_packet(&query);
		rcu_read_unlock();
        return -1;
    }

    if (node == NULL) {
        debug_ns("Requested name not found, creating empty response.\n");
        if (dnss_create_response(query, NULL, 0, &response) != 0) {
            dnss_destroy_packet(&query);
            dnss_destroy_packet(&response);
			rcu_read_unlock();
            return -1;
        }
    } else {
        debug_ns("Requested name found.\n");
		const ldns_rr_list *answers = zn_find_rrset(
				node, query->questions[0].qtype);
		if (dnss_create_response(query, answers, ldns_rr_list_rr_count(answers),
								 &response) != 0) {
            dnss_destroy_packet(&query);
            dnss_destroy_packet(&response);
			rcu_read_unlock();
            return -1;
        }
    }

	// end of RCU read critical section (all data copied)
	node = NULL;
	rcu_read_unlock();

    debug_ns("Response ID: %u\n", response->header.id);

    if (dnss_wire_format(response, response_wire, rsize) != 0) {
        debug_ns("Response too long, returning SERVFAIL response.\n");
        if (dnss_create_error_response(query, &response) != 0) {
            dnss_destroy_packet(&query);
            dnss_destroy_packet(&response);
            return -1;
        }
        int res = dnss_wire_format(response, response_wire, rsize);
        assert(res != 0);
    }

    debug_ns("Returning response of size: %u.\n", *rsize);

    dnss_destroy_packet(&query);
    dnss_destroy_packet(&response);

    return 0;
}

/*----------------------------------------------------------------------------*/

void ns_destroy( ns_nameserver **nameserver )
{
    // do nothing with the zone database!
    free(*nameserver);
    *nameserver = NULL;
}
