/* Copyright 2001-2003 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"

/**
 * \file routerlist.c
 *
 * \brief Code to
 * maintain and access the global list of routerinfos for known
 * servers.
 **/

/****************************************************************************/

extern or_options_t options; /**< command-line and config-file options */

/* ********************************************************************** */

/* static function prototypes */
static routerinfo_t *router_pick_directory_server_impl(void);
static int router_resolve_routerlist(routerlist_t *dir);

/****************************************************************************/

/****
 * Functions to manage and access our list of known routers. (Note:
 * dirservers maintain a separate, independent list of known router
 * descriptors.)
 *****/

/** Global list of all of the routers that we, as an OR or OP, know about. */
static routerlist_t *routerlist = NULL;

extern int has_fetched_directory; /**< from main.c */

/** Try to find a running dirserver.  If there are no running dirservers
 * in our routerlist, reload the routerlist and try again. */
routerinfo_t *router_pick_directory_server(void) {
  routerinfo_t *choice;

  choice = router_pick_directory_server_impl();
  if(!choice) {
    log_fn(LOG_WARN,"No dirservers known. Reloading and trying again.");
    has_fetched_directory=0; /* reset it */
    routerlist_clear_trusted_directories();
    if(options.RouterFile) {
      if(router_load_routerlist_from_file(options.RouterFile, 1) < 0)
        return NULL;
    } else {
      if(config_assign_default_dirservers() < 0)
        return NULL;
    }
    /* give it another try */
    choice = router_pick_directory_server_impl();
  }
  return choice;
}

/** Pick a random running router that's a trusted dirserver from our
 * routerlist. */
static routerinfo_t *router_pick_directory_server_impl(void) {
  int i;
  routerinfo_t *router;
  smartlist_t *sl;

  if(!routerlist)
    return NULL;

  /* Find all the running dirservers we know about. */
  sl = smartlist_create();
  for(i=0;i< smartlist_len(routerlist->routers); i++) {
    router = smartlist_get(routerlist->routers, i);
    if(router->is_running && router->is_trusted_dir) {
      tor_assert(router->dir_port > 0);
      smartlist_add(sl, router);
    }
  }

  router = smartlist_choose(sl);
  smartlist_free(sl);

  if(router)
    return router;
  log_fn(LOG_INFO,"No dirservers are reachable. Trying them all again.");

  /* No running dir servers found? go through and mark them all as up,
   * so we cycle through the list again. */
  sl = smartlist_create();
  for(i=0; i < smartlist_len(routerlist->routers); i++) {
    router = smartlist_get(routerlist->routers, i);
    if(router->is_trusted_dir) {
      tor_assert(router->dir_port > 0);
      router->is_running = 1;
      smartlist_add(sl, router);
    }
  }
  router = smartlist_choose(sl);
  smartlist_free(sl);
  if(!router)
    log_fn(LOG_WARN,"No dirservers in directory! Returning NULL.");
  return router;
}

/** Return 0 if \exists an authoritative dirserver that's currently
 * thought to be running, else return 1.
 */
int all_directory_servers_down(void) {
  int i;
  routerinfo_t *router;
  if(!routerlist)
    return 1; /* if no dirservers, I guess they're all down */
  for(i=0;i< smartlist_len(routerlist->routers); i++) {
    router = smartlist_get(routerlist->routers, i);
    if(router->is_running && router->is_trusted_dir) {
      tor_assert(router->dir_port > 0);
      return 0;
    }
  }
  return 1;
}

/** Given a comma-and-whitespace separated list of nicknames, see which
 * nicknames in <b>list</b> name routers in our routerlist that are
 * currently running.  Add the routerinfos for those routers to <b>sl</b>.
 */
void add_nickname_list_to_smartlist(smartlist_t *sl, const char *list) {
  const char *start,*end;
  char nick[MAX_NICKNAME_LEN+1];
  routerinfo_t *router;

  tor_assert(sl);
  tor_assert(list);

  while(isspace((int)*list) || *list==',') list++;

  start = list;
  while(*start) {
    end=start; while(*end && !isspace((int)*end) && *end != ',') end++;
    memcpy(nick,start,end-start);
    nick[end-start] = 0; /* null terminate it */
    router = router_get_by_nickname(nick);
    if (router) {
      if (router->is_running)
        smartlist_add(sl,router);
      else
        log_fn(LOG_WARN,"Nickname list includes '%s' which is known but down.",nick);
    } else
      log_fn(has_fetched_directory ? LOG_WARN : LOG_INFO,
             "Nickname list includes '%s' which isn't a known router.",nick);
    while(isspace((int)*end) || *end==',') end++;
    start = end;
  }
}

/** Add every router from our routerlist that is currently running to
 * <b>sl</b>.
 */
void router_add_running_routers_to_smartlist(smartlist_t *sl) {
  routerinfo_t *router;
  int i;

  if(!routerlist)
    return;

  for(i=0;i<smartlist_len(routerlist->routers);i++) {
    router = smartlist_get(routerlist->routers, i);
    if(router->is_running &&
       (!options.ORPort ||
        connection_get_by_identity_digest(router->identity_digest,
                                          CONN_TYPE_OR)))
      smartlist_add(sl, router);
  }
}

/** Return a random running router from the routerlist.  If any node
 * named in <b>preferred</b> is available, pick one of those.  Never pick a
 * node named in <b>excluded</b>, or whose routerinfo is in
 * <b>excludedsmartlist</b>, even if they are the only nodes available.
 */
routerinfo_t *router_choose_random_node(char *preferred, char *excluded,
                                        smartlist_t *excludedsmartlist)
{
  smartlist_t *sl, *excludednodes;
  routerinfo_t *choice;

  excludednodes = smartlist_create();
  add_nickname_list_to_smartlist(excludednodes,excluded);

  /* try the nodes in RendNodes first */
  sl = smartlist_create();
  add_nickname_list_to_smartlist(sl,preferred);
  smartlist_subtract(sl,excludednodes);
  if(excludedsmartlist)
    smartlist_subtract(sl,excludedsmartlist);
  choice = smartlist_choose(sl);
  smartlist_free(sl);
  if(!choice) {
    sl = smartlist_create();
    router_add_running_routers_to_smartlist(sl);
    smartlist_subtract(sl,excludednodes);
    if(excludedsmartlist)
      smartlist_subtract(sl,excludedsmartlist);
    choice = smartlist_choose(sl);
    smartlist_free(sl);
  }
  smartlist_free(excludednodes);
  if(!choice)
    log_fn(LOG_WARN,"No available nodes when trying to choose node. Failing.");
  return choice;
}

/** Return the router in our routerlist whose address is <b>addr</b> and
 * whose OR port is <b>port</b>. Return NULL if no such router is known.
 */
routerinfo_t *router_get_by_addr_port(uint32_t addr, uint16_t port) {
  int i;
  routerinfo_t *router;

  tor_assert(routerlist);

  for(i=0;i<smartlist_len(routerlist->routers);i++) {
    router = smartlist_get(routerlist->routers, i);
    if ((router->addr == addr) && (router->or_port == port))
      return router;
  }
  return NULL;
}

/** Return true iff the digest of <b>router</b>'s identity key,
 * encoded in hexadecimal, matches <b>hexdigest</b> (which is
 * optionally prefixed with a single dollar sign).  Return false if
 * <b>hexdigest</b> is malformed, or it doesn't match.  */
static INLINE int router_hex_digest_matches(routerinfo_t *router,
                                     const char *hexdigest)
{
  char digest[DIGEST_LEN];
  tor_assert(hexdigest);
  if (hexdigest[0] == '$')
    ++hexdigest;

  if (strlen(hexdigest) != HEX_DIGEST_LEN ||
      base16_decode(digest, DIGEST_LEN, hexdigest, HEX_DIGEST_LEN)<0)
    return 0;
  else
    return (!memcmp(digest, router->identity_digest, DIGEST_LEN));
}

/* Return true if <b>router</b>'s nickname matches <b>nickname</b>
 * (case-insensitive), or if <b>router's</b> identity key digest
 * matches a hexadecimal value stored in <b>nickname</b>.  Return
 * false otherwise.*/
int router_nickname_matches(routerinfo_t *router, const char *nickname)
{
  if (nickname[0]!='$' && !strcasecmp(router->nickname, nickname))
    return 1;
  else
    return router_hex_digest_matches(router, nickname);
}

/** Return the router in our routerlist whose (case-insensitive)
 * nickname or (case-sensitive) hexadecimal key digest is
 * <b>nickname</b>.  Return NULL if no such router is known.
 */
routerinfo_t *router_get_by_nickname(const char *nickname)
{
  int i, maybedigest;
  routerinfo_t *router;
  char digest[DIGEST_LEN];

  tor_assert(nickname);
  if (!routerlist)
    return NULL;
  if (nickname[0] == '$')
    return router_get_by_hexdigest(nickname);

  maybedigest = (strlen(nickname) == HEX_DIGEST_LEN) &&
    (base16_decode(digest,DIGEST_LEN,nickname,HEX_DIGEST_LEN) == 0);

  for(i=0;i<smartlist_len(routerlist->routers);i++) {
    router = smartlist_get(routerlist->routers, i);
    if (0 == strcasecmp(router->nickname, nickname) ||
        (maybedigest && 0 == memcmp(digest, router->identity_digest,
                                    DIGEST_LEN)))
      return router;
  }

  return NULL;
}

/** Return the router in our routerlist whose hexadecimal key digest
 * is <b>hexdigest</b>.  Return NULL if no such router is known. */
routerinfo_t *router_get_by_hexdigest(const char *hexdigest) {
  char digest[DIGEST_LEN];

  tor_assert(hexdigest);
  if (!routerlist)
    return NULL;
  if (hexdigest[0]=='$')
    ++hexdigest;
  if (strlen(hexdigest) != HEX_DIGEST_LEN ||
      base16_decode(digest,DIGEST_LEN,hexdigest,HEX_DIGEST_LEN) < 0)
    return NULL;

  return router_get_by_digest(digest);
}

/** Return the router in our routerlist whose 20-byte key digest
 * is <b>hexdigest</b>.  Return NULL if no such router is known. */
routerinfo_t *router_get_by_digest(const char *digest) {
  int i;
  routerinfo_t *router;

  tor_assert(digest);

  for(i=0;i<smartlist_len(routerlist->routers);i++) {
    router = smartlist_get(routerlist->routers, i);
    if (0 == memcmp(router->identity_digest, digest, DIGEST_LEN))
      return router;
  }

  return NULL;
}

/** Set *<b>prouterlist</b> to the current list of all known routers. */
void router_get_routerlist(routerlist_t **prouterlist) {
  *prouterlist = routerlist;
}

/** Free all storage held by <b>router</b>. */
void routerinfo_free(routerinfo_t *router)
{
  if (!router)
    return;

  tor_free(router->address);
  tor_free(router->nickname);
  tor_free(router->platform);
  if (router->onion_pkey)
    crypto_free_pk_env(router->onion_pkey);
  if (router->identity_pkey)
    crypto_free_pk_env(router->identity_pkey);
  exit_policy_free(router->exit_policy);
  free(router);
}

/** Allocate a fresh copy of <b>router</b> */
routerinfo_t *routerinfo_copy(const routerinfo_t *router)
{
  routerinfo_t *r;
  struct exit_policy_t **e, *tmp;

  r = tor_malloc(sizeof(routerinfo_t));
  memcpy(r, router, sizeof(routerinfo_t));

  r->address = tor_strdup(r->address);
  r->nickname = tor_strdup(r->nickname);
  r->platform = tor_strdup(r->platform);
  if (r->onion_pkey)
    r->onion_pkey = crypto_pk_dup_key(r->onion_pkey);
  if (r->identity_pkey)
    r->identity_pkey = crypto_pk_dup_key(r->identity_pkey);
  e = &r->exit_policy;
  while (*e) {
    tmp = tor_malloc(sizeof(struct exit_policy_t));
    memcpy(tmp,*e,sizeof(struct exit_policy_t));
    *e = tmp;
    (*e)->string = tor_strdup((*e)->string);
    e = & ((*e)->next);
  }
  return r;
}

/** Free all storage held by a routerlist <b>rl</b> */
void routerlist_free(routerlist_t *rl)
{
  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, r,
                    routerinfo_free(r));
  smartlist_free(rl->routers);
  tor_free(rl->software_versions);
  tor_free(rl);
}

/** Mark the router with ID <b>digest</b> as non-running in our routerlist. */
void router_mark_as_down(const char *digest) {
  routerinfo_t *router;
  tor_assert(digest);
  router = router_get_by_digest(digest);
  if(!router) /* we don't seem to know about him in the first place */
    return;
  log_fn(LOG_DEBUG,"Marking %s as down.",router->nickname);
  router->is_running = 0;
}

/** Add <b>router</b> to the routerlist, if we don't already have it.  Replace
 * older entries (if any) with the same name.  Note: Callers should not hold
 * their pointers to <b>router</b> after invoking this function; <b>router</b>
 * will either be inserted into the routerlist or freed.  Returns 0 if the
 * router was added; -1 if it was not.
 */
int router_add_to_routerlist(routerinfo_t *router) {
  int i;
  routerinfo_t *r;
  /* If we have a router with this name, and the identity key is the same,
   * choose the newer one. If the identity key has changed, drop the router.
   */
  for (i = 0; i < smartlist_len(routerlist->routers); ++i) {
    r = smartlist_get(routerlist->routers, i);
    /* XXXX008 should just compare digests instead. */
    if (!strcasecmp(router->nickname, r->nickname)) {
      if (!crypto_pk_cmp_keys(router->identity_pkey, r->identity_pkey)) {
        if (router->published_on > r->published_on) {
          log_fn(LOG_DEBUG, "Replacing entry for router '%s'",
                 router->nickname);
          /* Remember whether we trust this router as a dirserver. */
          if (r->is_trusted_dir)
            router->is_trusted_dir = 1;
          /* If the address hasn't changed; no need to re-resolve. */
          if (!strcasecmp(r->address, router->address))
            router->addr = r->addr;
          routerinfo_free(r);
          smartlist_set(routerlist->routers, i, router);
          return 0;
        } else {
          log_fn(LOG_DEBUG, "Skipping old entry for router '%s'",
                 router->nickname);
          /* If we now trust 'router', then we trust the one in the routerlist
           * too. */
          if (router->is_trusted_dir)
            r->is_trusted_dir = 1;
          /* Update the is_running status to whatever we were told. */
          r->is_running = router->is_running;
          routerinfo_free(router);
          return -1;
        }
      } else {
        /* XXXX008 It's okay to have two keys for a nickname as soon as
         * all the 007 clients are dead. */
        log_fn(LOG_WARN, "Identity key mismatch for router '%s'",
               router->nickname);
        routerinfo_free(router);
        return -1;
      }
    }
  }
  /* We haven't seen a router with this name before.  Add it to the end of
   * the list. */
  smartlist_add(routerlist->routers, router);
  return 0;
}

/** Remove any routers from the routerlist that are more than ROUTER_MAX_AGE
 * seconds old.
 *
 * (This function is just like dirserv_remove_old_servers. One day we should
 * merge them.)
 */
void
routerlist_remove_old_routers(void)
{
  int i;
  time_t cutoff;
  routerinfo_t *router;
  if (!routerlist)
    return;

  cutoff = time(NULL) - ROUTER_MAX_AGE;
  for (i = 0; i < smartlist_len(routerlist->routers); ++i) {
    router = smartlist_get(routerlist->routers, i);
    if (router->published_on < cutoff &&
      !router->dir_port) {
      /* Too old.  Remove it. But never remove dirservers! */
      log_fn(LOG_INFO,"Forgetting obsolete routerinfo for node %s.", router->nickname);
      routerinfo_free(router);
      smartlist_del(routerlist->routers, i--);
    }
  }
}

/*
 * Code to parse router descriptors and directories.
 */

/** Update the current router list with the one stored in
 * <b>routerfile</b>. If <b>trusted</b> is true, then we'll use
 * directory servers from the file. */
int router_load_routerlist_from_file(char *routerfile, int trusted)
{
  char *string;

  string = read_file_to_str(routerfile);
  if(!string) {
    log_fn(LOG_WARN,"Failed to load routerfile %s.",routerfile);
    return -1;
  }

  if(router_load_routerlist_from_string(string, trusted) < 0) {
    log_fn(LOG_WARN,"The routerfile itself was corrupt.");
    free(string);
    return -1;
  }
  /* dump_onion_keys(LOG_NOTICE); */

  free(string);
  return 0;
}

/** Mark all directories in the routerlist as nontrusted. */
void routerlist_clear_trusted_directories(void)
{
  if (!routerlist) return;
  SMARTLIST_FOREACH(routerlist->routers, routerinfo_t *, r,
                    r->is_trusted_dir = 0);
}

/** Helper function: read routerinfo elements from s, and throw out the
 * ones that don't parse and resolve.  Add all remaining elements to the
 * routerlist.  If <b>trusted</b> is true, then we'll use
 * directory servers from the string
 */
int router_load_routerlist_from_string(const char *s, int trusted)
{
  routerlist_t *new_list=NULL;

  if (router_parse_list_from_string(&s, &new_list, -1, NULL)) {
    log(LOG_WARN, "Error parsing router file");
    return -1;
  }
  if (trusted) {
    SMARTLIST_FOREACH(new_list->routers, routerinfo_t *, r,
                      if (r->dir_port) r->is_trusted_dir = 1);
  }
  if (routerlist) {
    SMARTLIST_FOREACH(new_list->routers, routerinfo_t *, r,
                      router_add_to_routerlist(r));
    smartlist_clear(new_list->routers);
    routerlist_free(new_list);
  } else {
    routerlist = new_list;
  }
  if (router_resolve_routerlist(routerlist)) {
    log(LOG_WARN, "Error resolving routerlist");
    return -1;
  }
  /* dump_onion_keys(LOG_NOTICE); */

  return 0;
}

/** Add to the current routerlist each router stored in the
 * signed directory <b>s</b>.  If pkey is provided, check the signature against
 * pkey; else check against the pkey of the signing directory server. */
int router_load_routerlist_from_directory(const char *s,
                                          crypto_pk_env_t *pkey)
{
  routerlist_t *new_list = NULL;
  check_software_version_against_directory(s, options.IgnoreVersion);
  if (router_parse_routerlist_from_directory(s, &new_list, pkey)) {
    log_fn(LOG_WARN, "Couldn't parse directory.");
    return -1;
  }
  if (routerlist) {
    SMARTLIST_FOREACH(new_list->routers, routerinfo_t *, r,
                      router_add_to_routerlist(r));
    smartlist_clear(new_list->routers);
    routerlist->published_on = new_list->published_on;
    tor_free(routerlist->software_versions);
    routerlist->software_versions = new_list->software_versions;
    new_list->software_versions = NULL;
    routerlist_free(new_list);
  } else {
    routerlist = new_list;
  }
  if (router_resolve_routerlist(routerlist)) {
    log_fn(LOG_WARN, "Error resolving routerlist");
    return -1;
  }
  if (options.AuthoritativeDir) {
    /* Learn about the descriptors in the directory. */
    dirserv_load_from_directory_string(s);
  } else {
    /* Remember the directory. */
    dirserv_set_cached_directory(s, routerlist->published_on);
  }
  return 0;
}

/** Helper function: resolve the hostname for <b>router</b>. */
static int
router_resolve(routerinfo_t *router)
{
  if (tor_lookup_hostname(router->address, &router->addr) != 0
      || !router->addr) {
    log_fn(LOG_WARN,"Could not get address for router %s (%s).",
           router->address, router->nickname);
    return -1;
  }
  router->addr = ntohl(router->addr); /* get it back into host order */

  return 0;
}

/** Helper function: resolve every router in rl, and ensure that our own
 * routerinfo is at the front.
 */
static int
router_resolve_routerlist(routerlist_t *rl)
{
  int i, remove;
  routerinfo_t *r;
  if (!rl)
    rl = routerlist;

  i = 0;
  if ((r = router_get_my_routerinfo())) {
    smartlist_insert(rl->routers, 0, routerinfo_copy(r));
    ++i;
  }

  for ( ; i < smartlist_len(rl->routers); ++i) {
    remove = 0;
    r = smartlist_get(rl->routers,i);
    if (router_is_me(r)) {
      remove = 1;
    } else if (r->addr) {
      /* already resolved. */
    } else if (router_resolve(r)) {
      log_fn(LOG_WARN, "Couldn't resolve router %s; not using", r->address);
      remove = 1;
    }
    if (remove) {
      routerinfo_free(r);
      smartlist_del_keeporder(rl->routers, i--);
    }
  }

  return 0;
}

/** Decide whether a given addr:port is definitely accepted, definitely
 * rejected, or neither by a given exit policy.  If <b>addr</b> is 0, we
 * don't know the IP of the target address.
 *
 * Returns -1 for "rejected", 0 for "accepted", 1 for "maybe" (since IP is
 * unknown).
 */
int router_compare_addr_to_exit_policy(uint32_t addr, uint16_t port,
                                       struct exit_policy_t *policy)
{
  int maybe_reject = 0;
  int maybe_accept = 0;
  int match = 0;
  int maybe = 0;
  struct in_addr in;
  struct exit_policy_t *tmpe;

  for(tmpe=policy; tmpe; tmpe=tmpe->next) {
//    log_fn(LOG_DEBUG,"Considering exit policy %s", tmpe->string);
    maybe = 0;
    if (!addr) {
      /* Address is unknown. */
      if (port >= tmpe->prt_min && port <= tmpe->prt_max) {
        /* The port definitely matches. */
        if (tmpe->msk == 0) {
          match = 1;
        } else {
          maybe = 1;
        }
      } else if (!port) {
        /* The port maybe matches. */
        maybe = 1;
      }
    } else {
      /* Address is known */
      if ((addr & tmpe->msk) == (tmpe->addr & tmpe->msk)) {
        if (port >= tmpe->prt_min && port <= tmpe->prt_max) {
          /* Exact match for the policy */
          match = 1;
        } else if (!port) {
          maybe = 1;
        }
      }
    }
    if (maybe) {
      if (tmpe->policy_type == EXIT_POLICY_REJECT)
        maybe_reject = 1;
      else
        maybe_accept = 1;
    }
    if (match) {
      in.s_addr = htonl(addr);
      log_fn(LOG_DEBUG,"Address %s:%d matches exit policy '%s'",
             inet_ntoa(in), port, tmpe->string);
      if(tmpe->policy_type == EXIT_POLICY_ACCEPT) {
        /* If we already hit a clause that might trigger a 'reject', than we
         * can't be sure of this certain 'accept'.*/
        return maybe_reject ? ADDR_POLICY_UNKNOWN : ADDR_POLICY_ACCEPTED;
      } else {
        return maybe_accept ? ADDR_POLICY_UNKNOWN : ADDR_POLICY_REJECTED;
      }
    }
  }
  /* accept all by default. */
  return maybe_reject ? ADDR_POLICY_UNKNOWN : ADDR_POLICY_ACCEPTED;
}

/** Return 1 if all running routers will reject addr:port, return 0 if
 * any might accept it. */
int router_exit_policy_all_routers_reject(uint32_t addr, uint16_t port) {
  int i;
  routerinfo_t *router;

  for (i=0;i<smartlist_len(routerlist->routers);i++) {
    router = smartlist_get(routerlist->routers, i);
    if (router->is_running && router_compare_addr_to_exit_policy(
             addr, port, router->exit_policy) != ADDR_POLICY_REJECTED)
      return 0; /* this one could be ok. good enough. */
  }
  return 1; /* all will reject. */
}

/** Return true iff <b>router</b> does not permit exit streams.
 */
int router_exit_policy_rejects_all(routerinfo_t *router) {
  return router_compare_addr_to_exit_policy(0, 0, router->exit_policy)
    == ADDR_POLICY_REJECTED;
}

/* Release all space held in <b>rr</b>. */
void running_routers_free(running_routers_t *rr)
{
  tor_assert(rr);
  if (rr->running_routers) {
    SMARTLIST_FOREACH(rr->running_routers, char *, s, tor_free(s));
    smartlist_free(rr->running_routers);
  }
  tor_free(rr);
}

/* Update the running/not-running status of every router in <b>list</b>, based
 * on the contents of <b>rr</b>. */
void routerlist_update_from_runningrouters(routerlist_t *list,
                                           running_routers_t *rr)
{
  int n_routers, n_names, i, j, running;
  routerinfo_t *router;
  const char *name;
  if (!list)
    return;
  if (list->published_on >= rr->published_on)
    return;
  if (list->running_routers_updated_on >= rr->published_on)
    return;

  n_routers = smartlist_len(list->routers);
  n_names = smartlist_len(rr->running_routers);
  for (i=0; i<n_routers; ++i) {
    running = 0;
    router = smartlist_get(list->routers, i);
    for (j=0; j<n_names; ++j) {
      name = smartlist_get(rr->running_routers, j);
      if (*name != '!') {
        if (router_nickname_matches(router, name)) {
          router->is_running = 1;
          break;
        }
      } else { /* *name == '!' */
        if (router_nickname_matches(router, name)) {
          router->is_running = 0;
          break;
        }
      }
    }
  }
  list->running_routers_updated_on = rr->published_on;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
