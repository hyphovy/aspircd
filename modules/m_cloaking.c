/*
 * Charybdis: an advanced ircd
 * m_cloaking.c: provide user hostname cloaking
 *
 * Written originally by nenolod, altered to use FNV by Elizabeth in 2008
 */

#include "stdinc.h"
#include "modules.h"
#include "hook.h"
#include "client.h"
#include "ircd.h"
#include "send.h"
#include "hash.h"
#include "s_conf.h"
#include "s_user.h"
#include "s_serv.h"
#include "numeric.h"

static int
_modinit(void)
{
    /* add the usermode to the available slot */
    user_modes['x'] = find_umode_slot();
    construct_umodebuf();

    return 0;
}

static void
_moddeinit(void)
{
    /* disable the umode and remove it from the available list */
    user_modes['x'] = 0;
    construct_umodebuf();
}

static void check_umode_change(void *data);
static void check_new_user(void *data);
mapi_hfn_list_av1 ip_cloaking_hfnlist[] = {
    { "umode_changed", (hookfn) check_umode_change },
    { "new_local_user", (hookfn) check_new_user },
    { NULL, NULL }
};

DECLARE_MODULE_AV1(ip_cloaking, _modinit, _moddeinit, NULL, NULL,
                   ip_cloaking_hfnlist, "$Revision$");

static void
distribute_hostchange(struct Client *client_p, char *newhost)
{
    if (newhost != client_p->orighost)
        sendto_one_numeric(client_p, RPL_HOSTHIDDEN, "%s :is now your hidden host",
                           newhost);
    else
        sendto_one_numeric(client_p, RPL_HOSTHIDDEN, "%s :hostname reset",
                           newhost);

    sendto_server(NULL, NULL,
                  CAP_EUID | CAP_TS6, NOCAPS, ":%s CHGHOST %s :%s",
                  use_id(&me), use_id(client_p), newhost);
    sendto_server(NULL, NULL,
                  CAP_TS6, CAP_EUID, ":%s ENCAP * CHGHOST %s :%s",
                  use_id(&me), use_id(client_p), newhost);

    change_nick_user_host(client_p, client_p->name, client_p->username, newhost, 0, "Changing host");

    if (newhost != client_p->orighost)
        SetDynSpoof(client_p);
    else
        ClearDynSpoof(client_p);
}

static void
do_host_cloak_ip(const char *inbuf, char *outbuf)
{
    /* None of the characters in this table can be valid in an IP */
    char chartable[] = "ghijklmnopqrstuvwxyz";
    char *tptr;
    uint32_t accum = fnv_hash((const unsigned char*) inbuf, 32);
    int sepcount = 0;
    int totalcount = 0;
    int ipv6 = 0;

    rb_strlcpy(outbuf, inbuf, HOSTLEN + 1);

    if (strchr(outbuf, ':')) {
        ipv6 = 1;

        /* Damn you IPv6...
         * We count the number of colons so we can calculate how much
         * of the host to cloak. This is because some hostmasks may not
         * have as many octets as we'd like.
         *
         * We have to do this ahead of time because doing this during
         * the actual cloaking would get ugly
         */
        for (tptr = outbuf; *tptr != '\0'; tptr++)
            if (*tptr == ':')
                totalcount++;
    } else if (!strchr(outbuf, '.'))
        return;

    for (tptr = outbuf; *tptr != '\0'; tptr++) {
        if (*tptr == ':' || *tptr == '.') {
            sepcount++;
            continue;
        }

        if (ipv6 && sepcount < totalcount / 2)
            continue;

        if (!ipv6 && sepcount < 2)
            continue;

        *tptr = chartable[(*tptr + accum) % 20];
        accum = (accum << 1) | (accum >> 31);
    }
}

static void
do_host_cloak_host(const char *inbuf, char *outbuf, int ipmask)
{
    int cyc;
    unsigned int hosthash = 1, hosthash2 = 1;
    unsigned int maxcycle = strlen(inbuf);
    int len1;
    const char *rest, *next;

    for (cyc = 0; cyc < maxcycle - 2; cyc += 5)
        hosthash *= (unsigned int) inbuf[cyc];

    /* safety: decrement ourselves two steps back */
    for (cyc = maxcycle - 1; cyc >= 1; cyc -= 2)
        hosthash2 *= (unsigned int) inbuf[cyc];

    /* lets do some bitshifting -- this pretty much destroys the IP
     * sequence, while still providing a checksum. exactly what
     * we're shooting for. --nenolod
     */
    hosthash += (hosthash2);
    hosthash2 += (hosthash);

    if (ipmask == 0) {
        rb_snprintf(outbuf, HOSTLEN, "%s-%X%X",
                    ServerInfo.network_name, hosthash2, hosthash);
        len1 = strlen(outbuf);
        rest = strchr(inbuf, '.');
        if (rest == NULL)
            rest = ".";
        /* try to avoid truncation -- jilles */
        while (len1 + strlen(rest) >= HOSTLEN && (next = strchr(rest + 1, '.')) != NULL)
            rest = next;
        rb_strlcat(outbuf, rest, HOSTLEN);
    } else
        rb_snprintf(outbuf, HOSTLEN, "%s-%X.%X.IP",
                    ServerInfo.network_name, hosthash2, hosthash);
}

static void
check_umode_change(void *vdata)
{
    hook_data_umode_changed *data = (hook_data_umode_changed *)vdata;
    struct Client *source_p = data->client;

    if (!MyClient(source_p))
        return;

    /* didn't change +x umode, we don't need to do anything */
    if (!((data->oldumodes ^ source_p->umodes) & user_modes['x']))
        return;

    if (source_p->umodes & user_modes['x']) {
        if (IsIPSpoof(source_p) || source_p->localClient->mangledhost == NULL || (IsDynSpoof(source_p) && strcmp(source_p->host, source_p->localClient->mangledhost))) {
            source_p->umodes &= ~user_modes['x'];
            return;
        }
        if (strcmp(source_p->host, source_p->localClient->mangledhost)) {
            distribute_hostchange(source_p, source_p->localClient->mangledhost);
        } else /* not really nice, but we need to send this numeric here */
            sendto_one_numeric(source_p, RPL_HOSTHIDDEN, "%s :is now your hidden host",
                               source_p->host);
    } else if (!(source_p->umodes & user_modes['x'])) {
        if (source_p->localClient->mangledhost != NULL &&
            !strcmp(source_p->host, source_p->localClient->mangledhost)) {
            distribute_hostchange(source_p, source_p->orighost);
        }
    }
}

static void
check_new_user(void *vdata)
{
    struct Client *source_p = (void *)vdata;

    if (IsIPSpoof(source_p)) {
        source_p->umodes &= ~user_modes['x'];
        return;
    }
    source_p->localClient->mangledhost = rb_malloc(HOSTLEN + 1);
    if (!irccmp(source_p->orighost, source_p->sockhost))
        do_host_cloak_ip(source_p->orighost, source_p->localClient->mangledhost);
    else
        do_host_cloak_host(source_p->orighost, source_p->localClient->mangledhost, 0);
    if (IsDynSpoof(source_p))
        source_p->umodes &= ~user_modes['x'];
    if (source_p->umodes & user_modes['x']) {
        rb_strlcpy(source_p->host, source_p->localClient->mangledhost, sizeof(source_p->host));
        if (irccmp(source_p->host, source_p->orighost))
            SetDynSpoof(source_p);
    }
}

