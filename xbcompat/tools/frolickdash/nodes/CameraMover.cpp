// CameraMover: eased camera moves between Viewpoints, the way Frolick
// flies between its pads (550 ms, ease-in-out quadratic).  The engine's
// own Viewpoint binding only offers an instant jump or a fixed linear
// one-second glide.
//
//   DEF theCameraMover CameraMover { duration 0.55  function OnArrive() {} }
//   theCameraMover.GoTo(vpCatalog);   // vpCatalog is a Viewpoint
//
// The Viewpoint keeps its VRML meaning: position with z pointing out of
// the screen and an axis-angle orientation, converted exactly as
// CViewpoint::Bind does.
#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"
#include "Camera.h"
#include "viewpoint.h"

extern CCamera theCamera;

class CCameraMover : public CNode
{
    DECLARE_NODE(CCameraMover, CNode)
public:
    CCameraMover();

    float m_duration;
    bool m_moving;

    void Advance(float nSeconds);
    void GoTo(CObject* pViewpoint);
    void Jump(CObject* pViewpoint);
    float Progress();

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()

protected:
    bool Target(CObject* pObject, D3DXVECTOR3* pPosition, D3DXQUATERNION* pOrientation, float* pFov);
    void SetFov(float fov);

    XTIME m_startTime;
    D3DXVECTOR3 m_fromPosition, m_toPosition;
    D3DXQUATERNION m_fromOrientation, m_toOrientation;
    // The projection takes its field of view from the bound Viewpoint, so a
    // flight to a Viewpoint with a different one lerps the bound Viewpoint's
    // value; m_boundFov keeps what it was authored with, for a flight back.
    float m_fromFov, m_toFov, m_curFov, m_boundFov;
};

IMPLEMENT_NODE("CameraMover", CCameraMover, CNode)

START_NODE_PROPS(CCameraMover, CNode)
    NODE_PROP(pt_number, CCameraMover, duration)
    NODE_PROP(pt_boolean, CCameraMover, moving)
END_NODE_PROPS()

START_NODE_FUN(CCameraMover, CNode)
    NODE_FUN_VO(GoTo)
    NODE_FUN_VO(Jump)
    NODE_FUN_NV(Progress)
END_NODE_FUN()

CCameraMover::CCameraMover() : m_duration(0.55f), m_moving(false), m_startTime(0.0f),
    m_fromFov(0.0f), m_toFov(0.0f), m_curFov(-1.0f), m_boundFov(0.0f)
{
}

void CCameraMover::SetFov(float fov)
{
    m_curFov = fov;
    if (theApp.m_pViewpoint != NULL && theApp.m_pViewpoint->m_fieldOfView != fov)
    {
        theApp.m_pViewpoint->m_fieldOfView = fov;
        theApp.m_bProjectionDirty = true;
    }
}

bool CCameraMover::Target(CObject* pObject, D3DXVECTOR3* pPosition, D3DXQUATERNION* pOrientation, float* pFov)
{
    if (pObject == NULL)
        return false;
    pObject = pObject->Deref();
    if (pObject == NULL || !((CNode*)pObject)->IsKindOf(NODE_CLASS(CViewpoint)))
        return false;
    CViewpoint* pVp = (CViewpoint*)pObject;
    *pPosition = pVp->m_position;
    pPosition->z = -pPosition->z;
    D3DXQuaternionRotationAxis(pOrientation, (D3DXVECTOR3*)&pVp->m_orientation, -pVp->m_orientation.w);
    if (m_curFov < 0.0f)
        m_curFov = m_boundFov = theApp.m_pViewpoint != NULL ? theApp.m_pViewpoint->m_fieldOfView : D3DX_PI / 2.0f;
    *pFov = pVp == theApp.m_pViewpoint ? m_boundFov : pVp->m_fieldOfView;
    return true;
}

void CCameraMover::GoTo(CObject* pViewpoint)
{
    if (!Target(pViewpoint, &m_toPosition, &m_toOrientation, &m_toFov))
        return;
    m_fromPosition = theCamera.m_position;
    m_fromOrientation = theCamera.m_orientation;
    m_fromFov = m_curFov;
    m_startTime = XAppGetNow();
    m_moving = true;
    if (m_duration <= 0.0f)
        Advance(0.0f);
}

void CCameraMover::Jump(CObject* pViewpoint)
{
    D3DXVECTOR3 position;
    D3DXQUATERNION orientation;
    float fov;
    if (!Target(pViewpoint, &position, &orientation, &fov))
        return;
    m_moving = false;
    theCamera.Set(&position, &orientation, 0.0f);
    SetFov(fov);
}

float CCameraMover::Progress()
{
    if (!m_moving)
        return 1.0f;
    float t = m_duration > 0.0f ? (float)(XAppGetNow() - m_startTime) / m_duration : 1.0f;
    return t > 1.0f ? 1.0f : t;
}

void CCameraMover::Advance(float nSeconds)
{
    if (!m_moving)
        return;
    float t = Progress();
    // ease-in-out quadratic, as Frolick's camera moves
    float f = t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    D3DXVECTOR3 position;
    D3DXQUATERNION orientation;
    D3DXVec3Lerp(&position, &m_fromPosition, &m_toPosition, f);
    D3DXQuaternionSlerp(&orientation, &m_fromOrientation, &m_toOrientation, f);
    theCamera.Set(&position, &orientation, 0.0f);
    SetFov(m_fromFov + (m_toFov - m_fromFov) * f);
    if (t >= 1.0f)
    {
        m_moving = false;
        CallFunction(this, _T("OnArrive"));
    }
}
