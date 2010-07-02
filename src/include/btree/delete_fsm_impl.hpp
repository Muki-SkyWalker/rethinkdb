
#ifndef __BTREE_DELETE_FSM_IMPL_HPP__
#define __BTREE_DELETE_FSM_IMPL_HPP__

#include "utils.hpp"
#include "cpu_context.hpp"

template <class config_t>
void btree_delete_fsm<config_t>::init_delete(int _key) {
    key = _key;
    state = start_transaction;
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_start_transaction(event_t *event) {
    assert(state == start_transaction);

    /* Either start a new transaction or retrieve the one we started. */
    assert(transaction == NULL);
    if (event == NULL) {
        transaction = cache->begin_transaction(rwi_write, this);
    } else {
        assert(event->buf); // We shouldn't get a callback unless this is valid
        transaction = (typename config_t::transaction_t *)event->buf;
    }

    /* Determine our forward progress based on our new state. */
    if (transaction) {
        state = acquire_superblock;
        return btree_fsm_t::transition_ok;
    } else {
        return btree_fsm_t::transition_incomplete; // Flush lock is held.
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_acquire_superblock(event_t *event) {
    printf("Acquire superblock\n");
    assert(state == acquire_superblock);

    buf_t *buf = NULL;
    if(event == NULL) {
        // First entry into the FSM; try to grab the superblock.
        block_id_t superblock_id = cache->get_superblock_id();
        buf = transaction->acquire(superblock_id, rwi_read, this);
    } else {
        // We already tried to grab the superblock, and we're getting
        // a cache notification about it.
        assert(event->buf);
        buf = (buf_t *)event->buf;
    }
    
    if(buf) {
        // Got the superblock buffer (either right away or through
        // cache notification). Grab the root id, and move on to
        // acquiring the root.
        node_id = btree_fsm_t::get_root_id(buf->ptr());
        buf->release();
        state = acquire_root;
        return btree_fsm_t::transition_ok;
    } else {
        // Can't get the superblock buffer right away. Let's wait for
        // the cache notification.
        return btree_fsm_t::transition_incomplete;
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_acquire_root(event_t *event) {
    printf("Acquire root\n");
    assert(state == acquire_root);
    
    // Make sure root exists
    if(cache_t::is_block_id_null(node_id)) {
        op_result = btree_not_found;
        state = delete_complete;
        return btree_fsm_t::transition_ok;
    }

    if(event == NULL) {
        // Acquire the actual root node
        buf = transaction->acquire(node_id, rwi_read, this);
    } else {
        // We already tried to grab the root, and we're getting a
        // cache notification about it.
        assert(event->buf);
        buf = (buf_t*)event->buf;
    }
    
    if(buf == NULL) {
        // Can't grab the root right away. Wait for a cache event.
        return btree_fsm_t::transition_incomplete;
    } else {
        // Got the root, move on to grabbing the node
        state = acquire_node;
        return btree_fsm_t::transition_ok;
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_acquire_node(event_t *event) {
    printf("Acquire node\n");
    assert(state == acquire_node);
    // Either we already have the node (then event should be NULL), or
    // we don't have the node (in which case we asked for it before,
    // and it should be getting to us via an event)
    //assert((buf && !event) || (!buf && event));

    if (!event) {
        buf = transaction->acquire(node_id, rwi_read, this);
    } else {
        assert(event->buf);
        buf = (buf_t*) event->buf;
    }

    if(buf)
        return btree_fsm_t::transition_ok;
    else
        return btree_fsm_t::transition_incomplete;
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_acquire_sibling(event_t *event) {
    printf("Acquire sibling\n");
    assert(state == acquire_sibling);

    if (!event) {
        assert(last_buf);
        node_t *last_node = (node_t *)last_buf->ptr();

        block_id_t sib_id;
        ((internal_node_t*)last_node)->sibling(key, &sib_id);
        sib_buf = transaction->acquire(sib_id, rwi_read, this);
    } else {
        assert(event->buf);
        sib_buf = (buf_t*) event->buf;
    }

    if(sib_buf) {
        printf("Done with acquire sibling\n");
        state = acquire_node;
        return btree_fsm_t::transition_ok;
    } else {
        printf("Acquire sibling incomplete\n");
        return btree_fsm_t::transition_incomplete;
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_transition(event_t *event) {
    printf("Do_transition\n");
    transition_result_t res = btree_fsm_t::transition_ok;

    // Make sure we've got either an empty or a cache event
    check("btree_fsm::do_transition - invalid event",
          !(!event || event->event_type == et_cache));

    // Update the cache with the event
    if(event && event->event_type == et_cache) {
        check("btree_delete _fsm::do_transition - invalid event", event->op != eo_read);
        check("Could not complete AIO operation",
              event->result == 0 ||
              event->result == -1);
    }

    // First, begin a transaction.
    if(res == btree_fsm_t::transition_ok && state == start_transaction) {
        res = do_start_transaction(event);
        event = NULL;
    }

    // Next, acquire the superblock (to get root node ID)
    if(res == btree_fsm_t::transition_ok && state == acquire_superblock) {
        assert(transaction); // We must have started our transaction by now.
        res = do_acquire_superblock(event);
        event = NULL;
    }
        
    // Then, acquire the root block
    if(res == btree_fsm_t::transition_ok && state == acquire_root) {
        res = do_acquire_root(event);
        event = NULL;
    }

    //Acquire a sibling
    if(res == btree_fsm_t::transition_ok && state == acquire_sibling) {
        res = do_acquire_sibling(event);
        event = NULL;
    }

    // Acquire nodes
    while(res == btree_fsm_t::transition_ok && state == acquire_node) {
        printf("Start acquire node loop\n");
        if(!buf) {
            printf("No buf\n");
            state = acquire_node;
            res = do_acquire_node(event);
            event = NULL;
            if(res != btree_fsm_t::transition_ok || state != acquire_node) {
                break;
            }
        }

        node_t* node = (node_t*)buf->ptr();

        //Deal with underful nodes if we find them
        if (node->is_underfull() && last_buf) {
            printf("Underfull node\n");
            if(!sib_buf) {
                state = acquire_sibling;
                res = do_acquire_sibling(event);
                printf("Sibling acquired: sib_buf = %li\n", (long int) sib_buf);
                event = NULL;
                continue;
            } else {
                // we have our sibling so we're ready to go
                node_t *sib_node = (node_t*)sib_buf->ptr();
                node_t *parent_node = (node_t*)last_buf->ptr();
                if(sib_node->is_underfull_or_min()) {
                    if (parent_node->is_singleton()) {
                        printf("Collapse time\n");
                        if (node->is_leaf())
                            assert(((internal_node_t*)parent_node)->collapse((leaf_node_t*) node, (leaf_node_t*) sib_node));
                        else
                            assert(((internal_node_t*)parent_node)->collapse((internal_node_t*) node, (internal_node_t*) sib_node));
                        //TODO these should be deleted when the api is ready
                        buf->release();
                        sib_buf->release();

                        sib_buf = NULL;
                        buf = last_buf;
                        last_buf = NULL;
                        buf->set_dirty();
                        node_id = last_node_id;
                    } else {
                        printf("Merge time\n");
                        if (node->is_leaf())
                            assert(((leaf_node_t*)node)->merge((internal_node_t*) last_buf->ptr(), (leaf_node_t*) sib_node));
                        else
                            assert(((internal_node_t*)node)->merge((internal_node_t*) last_buf->ptr(), (internal_node_t*) sib_node));
                        //TODO delete sib_buf, when delete is implemented
                        sib_buf->release();
                        sib_buf = NULL;
                    }
                } else {
                    printf("Level time\n");
                    if (node->is_leaf())
                        assert(((leaf_node_t*)node)->level((internal_node_t*) last_buf->ptr(), (leaf_node_t*) sib_node));
                    else
                        assert(((internal_node_t*)node)->level((internal_node_t*) last_buf->ptr(), (internal_node_t*) sib_node));
                    sib_buf->set_dirty();
                    sib_buf->release();
                    sib_buf = NULL;
                }
            }
        }

        //actually do some deleting 
        if (node->is_leaf()) {
            if(((leaf_node_t*)node)->remove(key)) {
                //key found, and value deleted
                buf->set_dirty();
                buf->release();
                op_result = btree_found;
            } else {
                //key not found, nothing deleted
                buf->release();
                op_result = btree_not_found;
            }
            if (last_buf)
                last_buf->release();
            state = delete_complete;
            res = btree_fsm_t::transition_ok;
            break;
        } else {
            if(!cache_t::is_block_id_null(last_node_id) && last_buf) {
                last_buf->release();
            }
            last_buf = buf;
            last_node_id = node_id;

            node_id = ((internal_node_t*)node)->lookup(key);
            buf = NULL;

            res = do_acquire_node(event);
            event = NULL;
        } 

        res = do_acquire_node(event);
        event = NULL;
    }

    // Finally, end our transaction.  This should always succeed immediately.
    if (res == btree_fsm_t::transition_ok && state == delete_complete) {
        bool committed __attribute__((unused)) = transaction->commit(this);
        state = committing;
        if (committed) {
            transaction = NULL;
            res = btree_fsm_t::transition_complete;
        }
        event = NULL;
    }

    // Finalize the transaction commit
    if(res == btree_fsm_t::transition_ok && state == committing) {
        if (event != NULL) {
            assert(event->event_type == et_commit);
            assert(event->buf == transaction);
            transaction = NULL;
            res = btree_fsm_t::transition_complete;
        }
    }

    assert(res != btree_fsm_t::transition_complete || is_finished());

    return res;
}

#endif // __BTREE_DELETE_FSM_IMPL_HPP__

