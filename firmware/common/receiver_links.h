#ifndef RECEIVER_LINKS_H
#define RECEIVER_LINKS_H

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/conn.h>

/* One gesture controller and one configuration client per receiver. */
enum gc_peer_role { GC_PEER_GESTURE, GC_PEER_CONFIG };
enum gc_claim_result {
    GC_CLAIM_DENIED = -1,
    GC_CLAIM_EXISTING = 0,
    GC_CLAIM_NEW = 1
};
struct gc_receiver_links {
    struct k_mutex lock;
    struct bt_conn *gesture;
    struct bt_conn *config;
    atomic_t connected;
};

static inline void gc_links_init(struct gc_receiver_links *links)
{
    k_mutex_init(&links->lock);
    links->gesture = NULL;
    links->config = NULL;
    atomic_set(&links->connected, 0);
}

static inline void gc_links_connected(struct gc_receiver_links *links)
{
    atomic_inc(&links->connected);
}

static inline int gc_links_count(struct gc_receiver_links *links)
{
    return atomic_get(&links->connected);
}

/* First relevant GATT operation assigns the peer's connection role. */
static inline enum gc_claim_result gc_links_claim(
    struct gc_receiver_links *links,
    struct bt_conn *conn, enum gc_peer_role role)
{
    struct bt_conn **slot;
    struct bt_conn **other;
    enum gc_claim_result result;

    k_mutex_lock(&links->lock, K_FOREVER);
    if (role == GC_PEER_GESTURE) {
        slot = &links->gesture;
        other = &links->config;
    } else {
        slot = &links->config;
        other = &links->gesture;
    }

    if (*other == conn || (*slot != NULL && *slot != conn)) {
        result = GC_CLAIM_DENIED;
    } else if (*slot == conn) {
        result = GC_CLAIM_EXISTING;
    } else {
        *slot = bt_conn_ref(conn);
        result = GC_CLAIM_NEW;
    }
    k_mutex_unlock(&links->lock);
    return result;
}

static inline bool gc_links_disconnected(
    struct gc_receiver_links *links, struct bt_conn *conn)
{
    bool gesture_lost = false;

    k_mutex_lock(&links->lock, K_FOREVER);
    if (links->gesture == conn) {
        links->gesture = NULL;
        gesture_lost = true;
        bt_conn_unref(conn);
    } else if (links->config == conn) {
        links->config = NULL;
        bt_conn_unref(conn);
    }
    atomic_dec(&links->connected);
    k_mutex_unlock(&links->lock);
    return gesture_lost;
}

#endif
