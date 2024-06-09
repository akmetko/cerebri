/*
 * Copyright CogniPilot Foundation 2023
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include <synapse_topic_list.h>

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "casadi/gen/b3rb.h"

#include <cerebri/core/casadi.h>

#define MY_STACK_SIZE 4096
#define MY_PRIORITY 4

LOG_MODULE_REGISTER(b3rb_position, CONFIG_CEREBRI_B3RB_LOG_LEVEL);

static K_THREAD_STACK_DEFINE(g_my_stack_area, MY_STACK_SIZE);

struct context {
    struct zros_node node;
    synapse_msgs_Status status;
    synapse_msgs_BezierTrajectory offboard_bezier_trajectory;
    synapse_msgs_Time offboard_clock_offset;
    synapse_msgs_Odometry estimator_odometry;
    synapse_msgs_Twist cmd_vel;
    struct zros_sub sub_status, sub_offboard_clock_offset, sub_estimator_odometry, sub_offboard_bezier_trajectory;
    struct zros_pub pub_cmd_vel;
    const double wheel_base;
    const double gain_along_track;
    const double gain_cross_track;
    const double gain_heading;
    atomic_t running;
    size_t stack_size;
    k_thread_stack_t* stack_area;
    struct k_thread thread_data;
};

static struct context g_ctx = {
    .status = synapse_msgs_Status_init_default,
    .offboard_bezier_trajectory = synapse_msgs_BezierTrajectory_init_default,
    .offboard_clock_offset = synapse_msgs_Time_init_default,
    .estimator_odometry = synapse_msgs_Odometry_init_default,
    .cmd_vel = {
        .has_angular = true,
        .has_linear = true,
        .linear = synapse_msgs_Vector3_init_default,
        .angular = synapse_msgs_Vector3_init_default,
    },
    .sub_status = {},
    .sub_offboard_clock_offset = {},
    .sub_estimator_odometry = {},
    .sub_offboard_bezier_trajectory = {},
    .pub_cmd_vel = {},
    .wheel_base = CONFIG_CEREBRI_B3RB_WHEEL_BASE_MM / 1000.0,
    .gain_along_track = CONFIG_CEREBRI_B3RB_GAIN_ALONG_TRACK / 1000.0,
    .gain_cross_track = CONFIG_CEREBRI_B3RB_GAIN_CROSS_TRACK / 1000.0,
    .gain_heading = CONFIG_CEREBRI_B3RB_GAIN_HEADING / 1000.0,
    .running = ATOMIC_INIT(0),
    .stack_size = MY_STACK_SIZE,
    .stack_area = g_my_stack_area,
    .thread_data = {},
};

static void b3rb_position_init(struct context* ctx)
{
    zros_node_init(&ctx->node, "b3rb_position");
    zros_sub_init(&ctx->sub_status, &ctx->node, &topic_status, &ctx->status, 10);
    zros_sub_init(&ctx->sub_offboard_clock_offset, &ctx->node, &topic_offboard_clock_offset, &ctx->offboard_clock_offset, 10);
    zros_sub_init(&ctx->sub_estimator_odometry, &ctx->node, &topic_estimator_odometry, &ctx->estimator_odometry, 10);
    zros_sub_init(&ctx->sub_offboard_bezier_trajectory, &ctx->node, &topic_offboard_bezier_trajectory, &ctx->offboard_bezier_trajectory, 10);
    zros_pub_init(&ctx->pub_cmd_vel, &ctx->node, &topic_cmd_vel, &ctx->cmd_vel);
    atomic_set(&ctx->running, 1);
}

static void b3rb_position_fini(struct context* ctx)
{
    atomic_set(&ctx->running, 0);
    zros_sub_fini(&ctx->sub_status);
    zros_sub_fini(&ctx->sub_offboard_clock_offset);
    zros_sub_fini(&ctx->sub_estimator_odometry);
    zros_sub_fini(&ctx->sub_offboard_bezier_trajectory);
    zros_pub_fini(&ctx->pub_cmd_vel);
    zros_node_fini(&ctx->node);
}

static void b3rb_position_stop(struct context* ctx)
{
    ctx->cmd_vel.linear.x = 0;
    ctx->cmd_vel.angular.z = 0;
}

// computes thrust/steering in auto mode
static void bezier_position_mode(struct context* ctx)
{
    // goal -> given position goal, find cmd_vel
    uint64_t time_start_nsec = ctx->offboard_bezier_trajectory.time_start;
    uint64_t time_stop_nsec = time_start_nsec;

    // get current time
    uint64_t time_nsec = k_uptime_get() * 1e6 + ctx->offboard_clock_offset.sec * 1e9 + ctx->offboard_clock_offset.nanosec;

    if (time_nsec < time_start_nsec) {
        LOG_WRN("time current: %" PRIu64
                " ns < time start: %" PRIu64
                "  ns, time out of range of trajectory\n",
            time_nsec, time_start_nsec);
        b3rb_position_stop(ctx);
        return;
    }

    // find current trajectory index, time_start, and time_stop
    int curve_index = 0;
    while (atomic_get(&ctx->running)) {

        // check if time handled by current trajectory
        if (time_nsec < ctx->offboard_bezier_trajectory.curves[curve_index].time_stop) {
            time_stop_nsec = ctx->offboard_bezier_trajectory.curves[curve_index].time_stop;
            if (curve_index > 0) {
                time_start_nsec = ctx->offboard_bezier_trajectory.curves[curve_index - 1].time_stop;
            }
            break;
        }

        // next index
        curve_index++;

        // check if index exceeds bounds
        if (curve_index >= ctx->offboard_bezier_trajectory.curves_count) {
            LOG_DBG("curve index exceeds bounds");
            b3rb_position_stop(ctx);
            return;
        }
    }

    double T = (time_stop_nsec - time_start_nsec) * 1e-9;
    double t = (time_nsec - time_start_nsec) * 1e-9;
    double x, y, psi, V, omega = 0;
    double e[3] = {}; // e_x, e_y, e_theta

    double PX[6], PY[6];
    for (int i = 0; i < 6; i++) {
        PX[i] = ctx->offboard_bezier_trajectory.curves[curve_index].x[i];
        PY[i] = ctx->offboard_bezier_trajectory.curves[curve_index].y[i];
    }

    /* bezier6_rover:(t,T,PX[1x6],PY[1x6],L)->(x,y,psi,V,omega) */
    {
        CASADI_FUNC_ARGS(bezier6_rover);
        args[0] = &t;
        args[1] = &T;
        args[2] = PX;
        args[3] = PY;
        res[0] = &x;
        res[1] = &y;
        res[2] = &psi;
        res[3] = &V;
        res[4] = &omega;
        CASADI_FUNC_CALL(bezier6_rover);
    }

    /* se2_error:(p[3],r[3])->(error[3]) */
    {
        double p[3], r[3];

        // vehicle position
        p[0] = ctx->estimator_odometry.pose.pose.position.x;
        p[1] = ctx->estimator_odometry.pose.pose.position.y;
        p[2] = 2 * atan2(ctx->estimator_odometry.pose.pose.orientation.z, ctx->estimator_odometry.pose.pose.orientation.w);

        // reference position
        r[0] = x;
        r[1] = y;
        r[2] = psi;

        // call function
        CASADI_FUNC_ARGS(se2_error);
        args[0] = p;
        args[1] = r;
        res[0] = e;
        CASADI_FUNC_CALL(se2_error);
    }

    // compute twist
    ctx->cmd_vel.linear.x = V + ctx->gain_along_track * e[0];
    ctx->cmd_vel.angular.z = omega + ctx->gain_cross_track * e[1] + ctx->gain_heading * e[2];
}

static void b3rb_position_run(void* p0, void* p1, void* p2)
{
    LOG_INF("init");
    struct context* ctx = p0;
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);

    b3rb_position_init(ctx);

    struct k_poll_event events[] = {
        *zros_sub_get_event(&ctx->sub_estimator_odometry),
    };

    while (true) {

        int rc = 0;
        rc = k_poll(events, ARRAY_SIZE(events), K_MSEC(1000));
        if (rc != 0) {
            LOG_DBG("pos not receiving estimator odometry");
            continue;
        }

        if (zros_sub_update_available(&ctx->sub_offboard_bezier_trajectory)) {
            zros_sub_update(&ctx->sub_offboard_bezier_trajectory);
        }

        if (zros_sub_update_available(&ctx->sub_status)) {
            zros_sub_update(&ctx->sub_status);
        }

        if (zros_sub_update_available(&ctx->sub_estimator_odometry)) {
            zros_sub_update(&ctx->sub_estimator_odometry);
        }

        if (zros_sub_update_available(&ctx->sub_offboard_clock_offset)) {
            zros_sub_update(&ctx->sub_offboard_clock_offset);
        }

        if (ctx->status.mode == synapse_msgs_Status_Mode_MODE_BEZIER) {
            bezier_position_mode(ctx);
            zros_pub_update(&ctx->pub_cmd_vel);
        }
    }

    b3rb_position_fini(ctx);
}

static int start(struct context* ctx)
{
    k_tid_t tid = k_thread_create(&ctx->thread_data, ctx->stack_area,
        ctx->stack_size,
        b3rb_position_run,
        ctx, NULL, NULL,
        MY_PRIORITY, 0, K_FOREVER);
    k_thread_name_set(tid, "b3rb_position");
    k_thread_start(tid);
    return 0;
}

static int b3rb_position_cmd_handler(const struct shell* sh,
    size_t argc, char** argv, void* data)
{
    struct context* ctx = data;
    if (argc != 1) {
        LOG_ERR("must have one argument");
        return -1;
    }

    if (strcmp(argv[0], "start") == 0) {
        if (atomic_get(&ctx->running)) {
            shell_print(sh, "already running");
        } else {
            start(ctx);
        }
    } else if (strcmp(argv[0], "stop") == 0) {
        if (atomic_get(&ctx->running)) {
            atomic_set(&ctx->running, 0);
        } else {
            shell_print(sh, "not running");
        }
    } else if (strcmp(argv[0], "status") == 0) {
        shell_print(sh, "running: %d", (int)atomic_get(&ctx->running));
    }
    return 0;
}

SHELL_SUBCMD_DICT_SET_CREATE(sub_b3rb_position, b3rb_position_cmd_handler,
    (start, &g_ctx, "start"),
    (stop, &g_ctx, "stop"),
    (status, &g_ctx, "status"));

SHELL_CMD_REGISTER(b3rb_position, &sub_b3rb_position, "b3rb position arguments", NULL);

static int b3rb_position_sys_init(void)
{
    return start(&g_ctx);
};

SYS_INIT(b3rb_position_sys_init, APPLICATION, 1);

// vi: ts=4 sw=4 et
