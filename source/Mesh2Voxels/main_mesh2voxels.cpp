#include "gvdb.h"
using namespace nvdb;

#include "main.h"
#include "nv_gui.h"
#include "GL/glew.h"

#include <vector>
using std::vector;

VolumeGVDB gvdb;

class Prgm : public NVPWindow
{
private: // gui
	void					initGUI(int w, int h);
	int						m_iMouseDown = -1;
	bool					m_bVisualizeTopology = false;

private: // opt
	void					revoxelize();
	void					visualizeTopology();

private: // model
	Vector3DF				m_vPivot;

	const float				m_fPartSizeScale = 500.0f;
	float					m_fPartSize = m_fPartSizeScale;
	
	const vector<float>		m_vVoxelSizeList = { 2.0f, 1.0f, 0.5f, 0.2f };
	int						m_iVoxelSizeSelect = 0;
	float					m_fVoxelSize = m_vVoxelSizeList[0];
	
	unsigned int			m_uChannel = 0u;

public: // render
	int						m_texScreen = -1;
	int						m_iShade = SHADE_VOXEL;
	
public: // inherit
	virtual bool			init() override;
	virtual void			display() override;
	virtual void			reshape(int w, int h) override;

public:
	virtual void			motion(int x, int y, int dx, int dy) override;
	virtual void			mouse(MouseButton button, ButtonAction action, int mods, int x, int y) override;
	virtual void			keyboardchar(unsigned char key, int mods, int x, int y) override;
};

Prgm prgm;

void Prgm::initGUI(int w, int h)
{
	clearGuis();
	setview2D(w, h);
	guiSetCallback([](int gui, float val) {
		switch(gui)
		{
		case 0:
			break;
		case 1:
			prgm.revoxelize();
			break;
		}
	});

	char* shadeName[SHADE_MAX];
	shadeName[SHADE_VOXEL] = "SHADE_VOXEL";
	shadeName[SHADE_SECTION2D] = "SHADE_SECTION2D";
	shadeName[SHADE_SECTION3D] = "SHADE_SECTION3D";
	shadeName[SHADE_EMPTYSKIP] = "SHADE_EMPTYSKIP";
	shadeName[SHADE_TRILINEAR] = "SHADE_TRILINEAR";
	shadeName[SHADE_TRICUBIC] = "SHADE_TRICUBIC";
	shadeName[SHADE_LEVELSET] = "SHADE_LEVELSET";
	shadeName[SHADE_VOLUME] = "SHADE_VOLUME";

	addGui(10, h - 70, 130, 20, "Topology", GUI_CHECK, GUI_BOOL, &m_bVisualizeTopology, 0.0f, 1.0f);
	addGui(10, h - 30, 130, 20, "VoxelSize", GUI_COMBO, GUI_INT, &m_iVoxelSizeSelect, 0.0f, 5.0f);
	for (auto it : m_vVoxelSizeList) addItem("");
	addGui(150, h - 30, 130, 20, "Shade", GUI_COMBO, GUI_INT, &m_iShade, 0.0, 7.0);
	for (int i = 0; i < 8; ++i) addItem(shadeName[i]);
}

void Prgm::revoxelize()
{
	gvdb.DestroyChannels();
	gvdb.AddChannel(0, T_FLOAT, 1);
	//gvdb.AddChannel(1, T_FLOAT, 1);

	m_fVoxelSize = m_vVoxelSizeList[m_iVoxelSizeSelect];

	Matrix4F xform;
	xform.Identity();

	Matrix4F t;
	xform *= t.Scale(m_fPartSize, m_fPartSize, m_fPartSize);
	xform *= t.Scale(1 / m_fVoxelSize, 1 / m_fVoxelSize, 1 / m_fVoxelSize);
	xform *= t.Translate(m_vPivot.X(), m_vPivot.Y(), m_vPivot.Z());
	
	gvdb.SetTransform(Vector3DF(0, 0, 0), Vector3DF(m_fVoxelSize, m_fVoxelSize, m_fVoxelSize), Vector3DF(0, 0, 0), Vector3DF(0, 0, 0));

	Model* model = gvdb.getScene()->getModel(0);
	gvdb.SolidVoxelize(0, model, &xform, 1.0, 0.5);
	Model* model1 = gvdb.getScene()->getModel(1);
	gvdb.SolidVoxelize(0, model1, &xform, 1.0, 0.5);
	gvdb.Measure(true);
}

void Prgm::visualizeTopology()
{
	start3D(gvdb.getScene()->getCamera());

	for (int lev = 0; lev < 5; ++lev)
	{
		auto cnt_node = gvdb.getNumNodes(lev);
		const Vector3DF& color = gvdb.getClrDim(lev);
		const Matrix4F& xform = gvdb.getTransform();

		for (decltype(cnt_node) n = 0; n < cnt_node; ++n)
		{
			Node* node = gvdb.getNodeAtLevel(n, lev);
			Vector3DF bmin = gvdb.getWorldMin(node);
			Vector3DF bmax = gvdb.getWorldMax(node);
			drawBox3DXform(bmin, bmax, color, xform);
		}
	}
}

bool Prgm::init()
{
	// gui
	int w = getWidth(), h = getHeight();
	init2D("arial");
	setview2D(w, h);

	// init
	gvdb.SetDebug(false);
	gvdb.SetVerbose(true);
	gvdb.SetCudaDevice(GVDB_DEV_FIRST);
	gvdb.Initialize();
	gvdb.StartRasterGL();
	gvdb.AddPath(ASSET_PATH);

	// load
	gvdb.getScene()->AddModel("lucy.obj", 1.0, 0, 0, 0);
	gvdb.CommitGeometry(0);
	gvdb.getScene()->AddModel("bunny.obj", 1.0, 0, 0, 0);
	gvdb.CommitGeometry(1);
	
	Model* mdl = gvdb.getScene()->getModel(0);
	auto aabb_min = mdl->objMin;
	auto aabb_max = mdl->objMax;
	auto aabb_size = aabb_max - aabb_min;
	auto aabb_ctr = aabb_size / 2.0f;
	m_vPivot.Set(-aabb_min.X(), -aabb_min.Y(), -aabb_min.Z());
	auto aabb_axis_max = max(max(aabb_size.X(), aabb_size.Y()), aabb_size.Z());
	m_fPartSize = m_fPartSizeScale / aabb_axis_max;

	// configure
	gvdb.Configure(3, 3, 3, 3, 5);
	gvdb.SetChannelDefault(16, 16, 1);

	// voxelize
	revoxelize();

	//
	gvdb.getScene()->SetSteps(0.5f, 0.5f, 0.5f);
	gvdb.getScene()->SetVolumeRange(0.5f, 0.0f, 1.0f);
	gvdb.getScene()->SetExtinct(-1.0f, 1.1f, 0.0f);
	gvdb.getScene()->SetCutoff(0.005f, 0.005f, 0.005f);
	gvdb.getScene()->SetShadowParams(0, 0, 0);
	gvdb.getScene()->LinearTransferFunc(0.0f, 0.5f, Vector4DF(0.0f, 0.0f, 0.0f, 0.0f), Vector4DF(1.0f, 1.0f, 1.0f, 0.5f));
	gvdb.getScene()->LinearTransferFunc(0.5f, 1.0f, Vector4DF(0.0f, 0.0f, 0.0f, 0.0f), Vector4DF(0.0f, 0.0f, 1.0f, 0.5f));
	gvdb.CommitTransferFunc();
	gvdb.getScene()->SetBackgroundClr(0.1f, 0.2f, 0.3f, 1.0f);

	// camera
	Camera3D* cam = new Camera3D;
	cam->setFov(50.0f);
	cam->setOrbit(Vector3DF(-45.f, 15.0f, 45.0f), aabb_ctr * m_fPartSize, 1500.f, 1.0f);
	gvdb.getScene()->SetCamera(cam);

	// light
	Light* lit = new Light;
	lit->setOrbit(Vector3DF(30.0f, 25.0f, 0.f), aabb_ctr * m_fPartSize, 500.0f, 1.0f);
	gvdb.getScene()->SetLight(0, lit);

	// render buffer
	gvdb.AddRenderBuf(0, w, h, sizeof(float));

	// screen
	glViewport(0, 0, w, h);
	createScreenQuadGL(&m_texScreen, w, h);
	
	initGUI(w, h);
	return true;
}

void Prgm::display()
{
	clearScreenGL();

	float h = getHeight();

	gvdb.Render(m_iShade, 0, 0);
	gvdb.ReadRenderTexGL(0, m_texScreen);
	renderScreenQuadGL(m_texScreen);

	if(m_bVisualizeTopology) visualizeTopology();

	draw3D();
	drawGui(0);
	draw2D();
	postRedisplay();
}

void Prgm::reshape(int w, int h)
{
	glViewport(0, 0, w, h);
	createScreenQuadGL(&m_texScreen, w, h);

	gvdb.ResizeRenderBuf(0, w, h, 4);

	initGUI(w, h);

	postRedisplay();
}

void Prgm::motion(int x, int y, int dx, int dy)
{
	Camera3D* cam = gvdb.getScene()->getCamera();
	Light* lit = gvdb.getScene()->getLight();

	switch (m_iMouseDown)
	{
	case NVPWindow::MOUSE_BUTTON_LEFT: {
		Vector3DF angle = cam->getAng();
		angle.x += dx * 0.2f;
		angle.y -= dy * 0.2f;
		cam->setOrbit(angle, cam->getToPos(), cam->getOrbitDist(), cam->getDolly());
		postRedisplay();
	} break;
	
	case NVPWindow::MOUSE_BUTTON_MIDDLE: {
		cam->moveRelative(float(dx) * cam->getOrbitDist() / 100.0f, float(-dy) * cam->getOrbitDist() / 100.0f, 0);
		postRedisplay();
	} break;
	
	case NVPWindow::MOUSE_BUTTON_RIGHT: {
		float dist = fabs(cam->getOrbitDist());
		dist -= 10 * dy;
		cam->setOrbit(cam->getAng(), cam->getToPos(), dist, cam->getDolly());
		postRedisplay();
	} break;
	
	default:
		break;
	}
}

void Prgm::mouse(MouseButton button, ButtonAction action, int mods, int x, int y)
{
	if (guiHandler(button, action, x, y)) return;

	m_iMouseDown = (action == NVPWindow::BUTTON_PRESS) ? button : -1;
}

void Prgm::keyboardchar(unsigned char key, int mods, int x, int y)
{
	auto tmp = Vector3DF(m_vPivot);

	switch (key)
	{
	}
}

int sample_main(int argc, const char** argv)
{
	return prgm.run("Mesh2Voxels", "M2V", argc, argv, 1024, 768, 4, 4);
}

void sample_print(int argc, char const* argv)
{

}
