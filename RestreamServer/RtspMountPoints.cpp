#include "RtspMountPoints.h"

#include <cassert>

#include <set>
#include <map>

#include "Config.h"

#include "Common/GlibPtr.h"
#include "Common/GstRtspServerPtr.h"
#include "Log.h"
#include "RtspRecordMediaFactory.h"
#include "RtspPlayMediaFactory.h"
#include "StaticSources.h"

static const gchar* RecordSuffix = "?record";

namespace RestreamServer
{

namespace
{

struct CxxPrivate
{
    MountPointsCallbacks callbacks;

    std::map<std::string, uint32_t> pathsRefs;
    std::map<GstRTSPClient*, std::set<std::string> > clientsToPaths;
};

}

struct _RtspMountPoints
{
    GstRTSPMountPoints parent_instance;

    uint64_t proxy;

    CxxPrivate* p;
};


G_DEFINE_TYPE(
    RtspMountPoints,
    rtsp_mount_points,
    GST_TYPE_RTSP_MOUNT_POINTS)


static gchar*
make_path(GstRTSPMountPoints* mountPoints, const GstRTSPUrl* url);


RtspMountPoints*
rtsp_mount_points_new(const MountPointsCallbacks& callbacks)
{
    RtspMountPoints* instance =
        _RTSP_MOUNT_POINTS(g_object_new(TYPE_RTSP_MOUNT_POINTS, NULL));

    if(instance)
        instance->p->callbacks = callbacks;

    return instance;
}

static void
rtsp_mount_points_class_init(RtspMountPointsClass* klass)
{
    GstRTSPMountPointsClass* gst_mount_points_klass =
        GST_RTSP_MOUNT_POINTS_CLASS(klass);

    gst_mount_points_klass->make_path = make_path;

    GObjectClass* object_klass = G_OBJECT_CLASS(klass);
    object_klass->finalize =
        [] (GObject* object) {
            RtspMountPoints* self = _RTSP_MOUNT_POINTS(object);
            delete self->p;
            self->p = nullptr;

            G_OBJECT_CLASS(rtsp_mount_points_parent_class)->finalize(object);
        };
}

static void
rtsp_mount_points_init(RtspMountPoints* self)
{
    self->proxy = 0;
    self->p = new CxxPrivate;
}

static void
client_closed(GstRTSPClient* client, gpointer userData)
{
    RtspMountPoints* self = _RTSP_MOUNT_POINTS(userData);

    CxxPrivate& p = *self->p;

    auto clientPathsIt = p.clientsToPaths.find(client);
    if(clientPathsIt == p.clientsToPaths.end()) {
        Log()->debug(
            "Client didn't use any path. client: {}",
            static_cast<const void*>(client));
        return;
    } else {
        for(const std::string& path: clientPathsIt->second) {
            auto pathRefsIt = p.pathsRefs.find(path);
            if(pathRefsIt == p.pathsRefs.end())
                Log()->critical("Inconsistent data in mount points reference counting");
            else {
                --pathRefsIt->second;
                if(0 == pathRefsIt->second) {
                    Log()->debug(
                        "Removing unused mount point. last client: {}, path: {}",
                        static_cast<const void*>(client), path);
                    gst_rtsp_mount_points_remove_factory(
                        GST_RTSP_MOUNT_POINTS(self),
                        path.data());
                    const std::string recordPath = path + RecordSuffix;
                    gst_rtsp_mount_points_remove_factory(
                        GST_RTSP_MOUNT_POINTS(self),
                        recordPath.data());
                    p.pathsRefs.erase(pathRefsIt);
                } else {
                    Log()->debug(
                        "Path ref count decreased. client: {}, path: {}, refs: {}",
                        static_cast<const void*>(client), path, pathRefsIt->second);
                }
            }
        }
        p.clientsToPaths.erase(clientPathsIt);
    }
}

static bool
authorize_access(
    RtspMountPoints* self,
    GstRTSPContext* context,
    const GstRTSPUrl* url)
{
    if(self->p->callbacks.authorizeAccess) {
        const gchar* user = nullptr;
        if(context->token) {
            user =
                gst_rtsp_token_get_string(
                    context->token,
                    GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE);
        }

        return self->p->callbacks.authorizeAccess(user ? user : "", url->abspath);
    } else
        return true;
}

static gchar*
make_path(GstRTSPMountPoints* mountPoints, const GstRTSPUrl* url)
{
    RtspMountPoints* self = _RTSP_MOUNT_POINTS(mountPoints);

    GstRTSPContext* context = gst_rtsp_context_get_current();
    assert(context);
    if(!context)
        return nullptr;

    if(!authorize_access(self, context, url))
        return nullptr;

    const std::string path = url->abspath;
    const bool isRecord = (g_strcmp0(url->query, "record") == 0);

    Log()->debug("make_path. client: {}, path: {}",
        static_cast<const void*>(context->client), path);

    static const URL splashSource =
        "rtsp://localhost:" STATIC_SERVER_PORT_STR BLUE;

    CxxPrivate& p = *self->p;

    auto pathRefsIt = p.pathsRefs.find(path);
    if(MAX_PATHS_COUNT > 0 &&
       pathRefsIt == p.pathsRefs.end() &&
       p.pathsRefs.size() >= MAX_PATHS_COUNT)
    {
        Log()->info(
            "Max paths count reached. client: {}, path: {}, count {}",
            static_cast<const void*>(context->client), path, MAX_PATHS_COUNT);

        return nullptr;
    }

    if(MAX_CLIENTS_PER_PATH > 0 &&
       pathRefsIt != p.pathsRefs.end() &&
       pathRefsIt->second >= MAX_CLIENTS_PER_PATH)
    {
        Log()->info(
            "Max clients count per path reached. client: {}, path: {}, count {}",
            static_cast<const void*>(context->client), path, MAX_PATHS_COUNT);

        return nullptr;
    }

    bool addPathRef = false;
    auto clientPathsIt = p.clientsToPaths.find(context->client);
    if(clientPathsIt == p.clientsToPaths.end()) {
        Log()->debug(
            "Path request from new client. client: {}, path: {}",
            static_cast<const void*>(context->client), path);

        g_signal_connect(context->client, "closed", GCallback(client_closed), mountPoints);
        p.clientsToPaths.emplace(context->client, std::set<std::string>{path});
        addPathRef = true;
    } else {
        Log()->debug(
            "Client requesting path. client: {}, path: {}",
            static_cast<const void*>(context->client), path);
        addPathRef = clientPathsIt->second.insert(path).second;
    }

    if(p.pathsRefs.end() == pathRefsIt) {
        Log()->debug(
            "Creating mount point. client: {}, path: {}",
            static_cast<const void*>(context->client), path);

        assert(addPathRef);

        const std::string proxyName =
            fmt::format("proxy{}", self->proxy++);

        RtspPlayMediaFactory* playFactory =
            rtsp_play_media_factory_new(splashSource, proxyName.c_str());
        RtspRecordMediaFactory* recordFactory =
            rtsp_record_media_factory_new(proxyName.c_str());

        gst_rtsp_mount_points_add_factory(
            mountPoints, url->abspath, GST_RTSP_MEDIA_FACTORY(playFactory));
        GCharPtr recordUrl(g_strconcat(url->abspath, RecordSuffix, nullptr));
        gst_rtsp_mount_points_add_factory(
            mountPoints, recordUrl.get(), GST_RTSP_MEDIA_FACTORY(recordFactory));

        p.pathsRefs.emplace(path, 1);
    } else {
        ++(pathRefsIt->second);
        Log()->debug(
            "Path ref count increased. client: {}, path: {}, refs: {}",
            static_cast<const void*>(context->client), path, pathRefsIt->second);
    }

    return
        isRecord ?
            g_strconcat(url->abspath, "?record", nullptr) :
            g_strdup(url->abspath);
}

}
