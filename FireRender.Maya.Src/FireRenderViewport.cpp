#include <cassert>
#include <sstream>
#include <thread>

#include "common.h"
#include "frWrap.h"
#include <maya/MNodeMessage.h>
#include <maya/MDagPath.h>
#include <maya/M3dView.h>
#include <maya/MFnCamera.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MImage.h>
#include <maya/MFileIO.h>
#include <maya/MGlobal.h>
#include <maya/MRenderView.h>
#include <maya/MAtomic.h>
#include <maya/MAnimControl.h>
#include <maya/MTextureManager.h>
#include "AutoLock.h"

#include "FireRenderContext.h"
#include "FireRenderViewport.h"
#include "FireRenderThread.h"

using namespace std;
using namespace std::chrono_literals;
using namespace MHWRender;
using namespace RPR;
using namespace FireMaya;

//#define HIGHLIGHT_TEXTURE_UPDATES	1	// debugging: every update will draw a color line on top of the rendered picture

// Life Cycle
// -----------------------------------------------------------------------------
FireRenderViewport::FireRenderViewport(const MString& panelName) :
	m_isRunning(false),
	m_useAnimationCache(true),
	m_pixelsUpdated(false),
	m_panelName(panelName),
	m_textureChanged(false),
	m_showDialogNeeded(false),
	m_closeDialogNeeded(false),
	m_createFailed(false)
{
	// Initialize.
	if (!initialize())
		m_createFailed = true;

	// Get the Maya 3D view.
	M3dView::getM3dViewFromModelPanel(panelName, m_view);

	// Add the RPR panel menu.
	addMenu();
}

// -----------------------------------------------------------------------------
FireRenderViewport::~FireRenderViewport()
{
	// Stop the render thread if required. Executed in context of the main thread.
	stop();
	// Now cleanup resources in context of rendering thread.
	// TODO: check - may be it is not required to call this in thread context, because if thread is not
	// running anymore - it is safe to call it from any thread.
	FireRenderThread::RunOnceProcAndWait([this]()
	{
		cleanUp();
	});
}


// Public Methods
// -----------------------------------------------------------------------------
// How Maya executes callbacks for refreshing viewport:
// FireRenderOverride::setup
//   -> FireRenderViewport::setup
//     -> FireRenderViewportBlit::updateTexture
// FireRenderOverride::startOperationIterator
// FireRenderOverride::nextRenderOperation (several times) - texture is displayed in viewport here
// FireRenderViewportManager::postRenderMsgCallback
//   -> FireRenderViewport::refresh
// FireRenderOverride::cleanup
MStatus FireRenderViewport::setup()
{
	MAIN_THREAD_ONLY;

	// Check if updating viewport's texture is required.
	// No action is required if GL interop is active: the shared OpenGL frame buffer is rendered directly.
	if (!m_context.isGLInteropActive() && m_pixelsUpdated)
	{
		// Acquire the pixels lock.
		AutoMutexLock pixelsLock(m_pixelsLock);

		// Update the Maya texture from the pixel data.
		updateTexture(m_pixels.data(), m_context.width(), m_context.height());
	}

	// Execute doSetup() in context of rendering thread
	auto status = FireRenderThread::RunOnceAndWait<MStatus>([this]() -> MStatus
	{
		return doSetup();
	});

	return status;
}

MStatus FireRenderViewport::doSetup()
{
	RPR_THREAD_ONLY;


	// Check for errors.
	if (m_error.check())
		return MStatus::kFailure;

	// Get the viewport dimensions.
	unsigned int width = 0;
	unsigned int height = 0;
	MStatus status = getSize(width, height);
	if (status != MStatus::kSuccess)
		return status;

	// Update render limits based on animation state.
	bool animating = MAnimControl::isPlaying() || MAnimControl::isScrubbing();
	m_context.updateLimits(animating);

	// Check if animation caching should be used.
	bool useAnimationCache =
		animating && m_useAnimationCache && !m_context.isGLInteropActive();

	// Stop the viewport render thread if using cached frames.
	if (m_isRunning && useAnimationCache)
		stop();

	// Check if the viewport size has changed.
	if (width != m_context.width() || height != m_context.height())
	{
		status = resize(width, height);
		if (status != MStatus::kSuccess)
			return status;
	}

	// Refresh the context if required.
	status = refreshContext();
	if (status != MStatus::kSuccess)
		return status;

	// Check for errors again - the render thread may have
	// encountered an error since the start of this method.
	if (m_error.check())
		return MStatus::kFailure;

	// Render a cached frame if required.
	if (useAnimationCache)
	{
		status = renderCached(width, height);
		if (status != MStatus::kSuccess)
			return status;
	}

	// Otherwise, ensure the render thread is running.
	else if (!m_isRunning)
		start();

	// Viewport setup complete.
	return MStatus::kSuccess;
}

// -----------------------------------------------------------------------------
void FireRenderViewport::removed(bool panelDestroyed)
{
	// Do nothing if the containing panel is being destroyed.
	if (panelDestroyed)
		return;

	// Otherwise, remove the panel menu.
	removeMenu();
}

// -----------------------------------------------------------------------------
bool FireRenderViewport::RunOnViewportThread()
{
	RPR_THREAD_ONLY;

	switch (m_context.state)
	{
		// The context is exiting.
	case FireRenderContext::StateExiting:
		return false;

		// The context is rendering.
	case FireRenderContext::StateRendering:
	{
		// Check if a render is required. Note that this code will be executed
		if (m_context.cameraAttributeChanged() ||	// camera changed
			m_context.needsRedraw() ||				// context needs redrawing
			m_context.keepRenderRunning())			// or we must render just because rendering is not yet completed
		{
			try
			{
				FireRenderContext::Lock lock(&m_context, "FireRenderContext::StateRendering"); // lock with constructor which will not change state

				// Perform a render iteration.
				{
					AutoMutexLock contextLock(m_contextLock);

					m_context.render(false);
					m_closeDialogNeeded = true;
				}

				// Lock pixels and read the frame buffer.
				{
					AutoMutexLock pixelsLock(m_pixelsLock);
					readFrameBuffer();
				}

				if (m_renderingErrors > 0)
					m_renderingErrors--;
			}
			catch (...)
			{
				m_view.scheduleRefresh();
				m_renderingErrors++;
				DebugPrint("Failed to Render Viewport: %d errors in a row!", m_renderingErrors);
				if (m_renderingErrors > 3)
					throw;
			}

			// Schedule a Maya viewport refresh.
			m_view.scheduleRefresh();
		}
		else
		{
			// Don't waste CPU time too much when not rendering
			this_thread::sleep_for(2ms);
		}

		return true;
	}

	case FireRenderContext::StatePaused:	// The context is paused.
	case FireRenderContext::StateUpdating:	// The context is updating.
	default:								// Handle all other cases.
		this_thread::sleep_for(5ms);
		return true;
	}
}

// -----------------------------------------------------------------------------
bool FireRenderViewport::start()
{
	// Stop before restarting if already running.
	if (m_isRunning)
		stop();

	// Check dimensions are valid.
	if (m_context.width() == 0 || m_context.height() == 0)
		return false;

	// Start rendering.
	{
		// We should lock context, otherwise another asynchronous lock could
		// change context's state, and rendering will stall in StateUpdating.
		FireRenderContext::Lock lock(&m_context, "FireRenderViewport::start"); // lock constructor which do not change state
		m_context.state = FireRenderContext::StateRendering;
	}

	m_isRunning = false;

	m_renderingErrors = 0;

	FireRenderThread::KeepRunning([this]()
	{
		try
		{
			m_isRunning = this->RunOnViewportThread();
		}
		catch (...)
		{
			m_isRunning = false;
			m_error.set(current_exception());
		}

		return m_isRunning;
	});

	return true;
}

// -----------------------------------------------------------------------------
bool FireRenderViewport::stop()
{
	MAIN_THREAD_ONLY;

	// should wait for thread
	// m_isRunning could be not updated when exiting Maya during rendering, so check for two conditions
	while (m_isRunning && FireRenderThread::IsThreadRunning())
	{
		//Run items queued for main thread
		FireRenderThread::RunItemsQueuedForTheMainThread();

		// terminate the thread
		m_context.state = FireRenderContext::StateExiting;
		this_thread::sleep_for(10ms); // 10.03.2017 - perhaps this is better than yield()
	}

	if (rcWarningDialog.shown && m_closeDialogNeeded)
		rcWarningDialog.close();

	return true;
}

// -----------------------------------------------------------------------------
void FireRenderViewport::setUseAnimationCache(bool value)
{
	m_useAnimationCache = value;
	m_view.scheduleRefresh();
}

// -----------------------------------------------------------------------------
void FireRenderViewport::setViewportRenderModel(int renderMode)
{
	FireRenderThread::RunOnceProcAndWait([this, renderMode]()
	{
		AutoMutexLock contextLock(m_contextLock);
		m_context.setRenderMode(static_cast<FireRenderContext::RenderMode>(renderMode));
		m_context.setDirty();
		m_view.scheduleRefresh();
	});
}

// -----------------------------------------------------------------------------
bool FireRenderViewport::useAnimationCache()
{
	return m_useAnimationCache;
}

// -----------------------------------------------------------------------------
void FireRenderViewport::clearTextureCache()
{
	m_textureCache.Clear();
	m_view.scheduleRefresh();
}

// -----------------------------------------------------------------------------
MStatus FireRenderViewport::cameraChanged(MDagPath& cameraPath)
{
	auto status = FireRenderThread::RunOnceAndWait<MStatus>([this, &cameraPath]() -> MStatus
	{

		AutoMutexLock contextLock(m_contextLock);

		try
		{
			m_context.setCamera(cameraPath, true);
			m_context.setDirty();
		}
		catch (...)
		{
			m_error.set(current_exception());
			return MStatus::kFailure;
		}

		return MStatus::kSuccess;
	});

	return status;
}

// -----------------------------------------------------------------------------
MStatus FireRenderViewport::refresh()
{
	if (rcWarningDialog.shown && m_closeDialogNeeded)
	{
		rcWarningDialog.close();
	}
	else if (m_showDialogNeeded)
	{
		m_showDialogNeeded = false;
		rcWarningDialog.show();
	}

	// Check for errors.
	if (m_error.check())
		return MStatus::kFailure;

	return MStatus::kSuccess;
}

// -----------------------------------------------------------------------------
void FireRenderViewport::preBlit()
{
	// If GL Interop is active, ensure that Maya
	// has exclusive access to the OpenGL frame
	// buffer before using it to draw to the viewport.
	if (m_context.isGLInteropActive())
		m_pixelsLock.lock();
}

// -----------------------------------------------------------------------------
void FireRenderViewport::postBlit()
{
	// Release the context lock after the shared
	// GL frame buffer has been drawn to the viewport.
	if (m_context.isGLInteropActive())
		m_pixelsLock.unlock();
}


// Private Methods
// -----------------------------------------------------------------------------
bool FireRenderViewport::initialize()
{
	return FireRenderThread::RunOnceAndWait<bool>([this]()
	{
		try
		{
			// Initialize the hardware texture.
			m_texture.texture = nullptr;
			m_textureDesc.setToDefault2DTexture();
			m_textureDesc.fFormat = MRasterFormat::kR32G32B32A32_FLOAT;

			// Initialize the RPR context.
			bool animating = MAnimControl::isPlaying() || MAnimControl::isScrubbing();
			bool glViewport = MRenderer::theRenderer()->drawAPIIsOpenGL();

			//enable AOV-COLOR so that it can be resolved and used properly
			m_context.enableAOV(RPR_AOV_COLOR);

			m_context.setInteractive(true);
			if (!m_context.buildScene(animating, true, glViewport))
				return false;
		}
		catch (...)
		{
			m_error.set(current_exception());
			return false;
		}
		return true;
	});
}

// -----------------------------------------------------------------------------
void FireRenderViewport::cleanUp()
{
	// Clean the RPR scene.
	m_context.cleanScene();

	// Delete the hardware backed texture.
	// Do not delete when exiting Maya - this will cause access violation
	// in texture manager.
	if (m_texture.texture && !gExitingMaya)
	{
		MRenderer* renderer = MRenderer::theRenderer();
		MTextureManager* textureManager = renderer->getTextureManager();

		textureManager->releaseTexture(m_texture.texture);
		m_texture.texture = nullptr;
	}
}

// -----------------------------------------------------------------------------
MStatus FireRenderViewport::getSize(unsigned int& width, unsigned int& height)
{
	// Get the viewport size from the renderer.
	MRenderer* renderer = MRenderer::theRenderer();
	MStatus status = renderer->outputTargetSize(width, height);
	if (status != MStatus::kSuccess)
		return status;

	// Clamp the maximum size to increase
	// performance and reduce memory usage.
	if (width > height && width > FRMAYA_GL_MAX_TEXTURE_SIZE)
	{
		height = height * FRMAYA_GL_MAX_TEXTURE_SIZE / width;
		width = FRMAYA_GL_MAX_TEXTURE_SIZE;
	}
	else if (height > FRMAYA_GL_MAX_TEXTURE_SIZE)
	{
		width = width * FRMAYA_GL_MAX_TEXTURE_SIZE / height;
		height = FRMAYA_GL_MAX_TEXTURE_SIZE;
	}

	// Success.
	return MStatus::kSuccess;
}

// -----------------------------------------------------------------------------
MStatus FireRenderViewport::resize(unsigned int width, unsigned int height)
{
	// Acquire the context and pixels locks.
	AutoMutexLock contextLock(m_contextLock);
	AutoMutexLock pixelsLock(m_pixelsLock);

	try
	{
		// Clear the texture cache - all frames
		// need to be re-rendered at the new size.
		m_textureCache.Clear();

		// Delete the existing hardware backed texture.
		if (m_texture.texture)
		{
			MRenderer* renderer = MRenderer::theRenderer();
			MTextureManager* textureManager = renderer->getTextureManager();
			textureManager->releaseTexture(m_texture.texture);
			m_texture.texture = nullptr;
		}

		if (m_context.isFirstIterationAndShadersNOTCached()) {
			//first iteration and shaders are _NOT_ cached
			m_closeDialogNeeded = false;
			m_showDialogNeeded = true;
		}

		// Resize the frame buffer.
		if (m_context.isGLInteropActive())
			resizeFrameBufferGLInterop(width, height);
		else
			resizeFrameBufferStandard(width, height);

		// Update the camera.
		M3dView mView;
		MStatus status = M3dView::getM3dViewFromModelPanel(m_panelName, mView);
		if (status != MStatus::kSuccess)
			return status;

		MDagPath cameraPath;
		status = mView.getCamera(cameraPath);
		if (status != MStatus::kSuccess)
			return status;

		if (cameraPath.isValid())
			m_context.setCamera(cameraPath, true);


		// Invalidate the context.
		m_context.setDirty();
	}
	catch (...)
	{
		m_error.set(current_exception());
		return MStatus::kFailure;
	}

	// Resize completed successfully.
	return MStatus::kSuccess;
}

// -----------------------------------------------------------------------------
void FireRenderViewport::resizeFrameBufferStandard(unsigned int width, unsigned int height)
{
	// Update the RPR context dimensions.
	m_context.resize(width, height, false);

	// Resize the pixel buffer that
	// will receive frame buffer data.
	m_pixels.resize(width * height);

	// Perform an initial frame buffer read and update the texture.
	readFrameBuffer();
	updateTexture(m_pixels.data(), width, height);
}

// -----------------------------------------------------------------------------
void FireRenderViewport::resizeFrameBufferGLInterop(unsigned int width, unsigned int height)
{
	// Resize the pixel buffer that
	// will receive frame buffer data.
	m_pixels.resize(width * height);
	clearPixels();

	// Perform an initial frame buffer read and update the texture.
	updateTexture(m_pixels.data(), width, height);

	// Get the GL texture.
	if (m_texture.texture != nullptr)
	{
		MTexture* texture = m_texture.texture;
		rpr_GLuint* glTexture = static_cast<rpr_GLuint*>(texture->resourceHandle());

		// Update the RPR context.
		m_context.resize(width, height, false, glTexture);
	}
}

// -----------------------------------------------------------------------------
void FireRenderViewport::clearPixels()
{
	RV_PIXEL zero;
	zero.r = 0;
	zero.g = 0;
	zero.b = 0;
	zero.a = 1;

	std::fill(m_pixels.begin(), m_pixels.end(), zero);
}

// -----------------------------------------------------------------------------
MStatus FireRenderViewport::renderCached(unsigned int width, unsigned int height)
{
	// Clear the pixels updated flag so the non-cached frame
	// buffer data doesn't get written to the texture post render.
	m_pixelsUpdated = false;

	try
	{
		// Get the frame hash.
		auto hash = m_context.GetStateHash();
		stringstream ss;
		ss << m_panelName.asChar() << ";" << size_t(hash);

		// Get the frame for the hash.
		auto& frame = m_textureCache[ss.str().c_str()];

		// Render the frame if required.
		if (frame.Resize(width, height))
		{
			AutoMutexLock contextLock(m_contextLock);

			m_context.render();
			readFrameBuffer(&frame);

			return updateTexture(frame.data(), width, height);
		}
		else // Otherwise, update the texture from the frame data.
		{
			return updateTexture(frame.data(), width, height);
		}
	}
	catch (...)
	{
		m_error.set(current_exception());
		return MStatus::kFailure;
	}
}

// -----------------------------------------------------------------------------
MStatus FireRenderViewport::refreshContext()
{
	RPR_THREAD_ONLY;

	if (!m_context.isDirty())
		return MStatus::kSuccess;

	try
	{
		m_context.Freshen(true);

		return MStatus::kSuccess;
	}
	catch (...)
	{
		m_error.set(current_exception());
		return MStatus::kFailure;
	}
}

// -----------------------------------------------------------------------------
void FireRenderViewport::readFrameBuffer(FireMaya::StoredFrame* storedFrame)
{
	// The resolved frame buffer is shared with the Maya viewport
	// when GL interop is active, so only the resolve step is required.
	if (m_context.isGLInteropActive())
	{
		m_context.frameBufferAOV_Resolved(RPR_AOV_COLOR);
		return;
	}

	// Read the frame buffer.
	RenderRegion region(0, m_context.width() - 1, 0, m_context.height() - 1);

	// Read to a cached frame if supplied.
	if (storedFrame)
	{
		m_context.readFrameBuffer(reinterpret_cast<RV_PIXEL*>(storedFrame->data()),
			RPR_AOV_COLOR, m_context.width(), m_context.height(), region, false);
	}

	// Otherwise, read to a temporary buffer.
	else
	{
		m_context.readFrameBuffer(m_pixels.data(), RPR_AOV_COLOR, m_context.width(), m_context.height(), region, false);

		// Flag as updated so the pixels will
		// be copied to the viewport texture.
		m_pixelsUpdated = true;
#if HIGHLIGHT_TEXTURE_UPDATES
		static const RV_PIXEL colors[6] =
		{
			{ 1, 0, 0, 1 },
			{ 0, 1, 0, 1 },
			{ 0, 0, 1, 1 },
			{ 1, 1, 0, 1 },
			{ 0, 1, 1, 1 },
			{ 1, 0, 1, 1 }
		};
		static int nn = 0;
		RV_PIXEL c = colors[nn];
		if (++nn == 6) nn = 0;
		LogPrint(">>> fill: %g %g %g", c.r, c.g, c.b);
		for (int i = 0; i < m_context.width() * 8; i += m_context.width())
		{
			for (int j = 0; j < 8; j++)
				m_pixels[i + j] = c;
		}
#endif // HIGHLIGHT_TEXTURE_UPDATES
	}
}

// -----------------------------------------------------------------------------
MStatus FireRenderViewport::updateTexture(void* data, unsigned int width, unsigned int height)
{
	// Create the hardware backed texture if required.
	if (!m_texture.texture)
	{
		// Update the texture description.
		m_textureDesc.setToDefault2DTexture();
		m_textureDesc.fWidth = width;
		m_textureDesc.fHeight = height;
		m_textureDesc.fDepth = 1;
		m_textureDesc.fBytesPerRow = 4 * sizeof(float) * width;
		m_textureDesc.fBytesPerSlice = m_textureDesc.fBytesPerRow * height;
		m_textureDesc.fFormat = MRasterFormat::kR32G32B32A32_FLOAT;

		// Create a new texture with the supplied data.
		MRenderer* renderer = MRenderer::theRenderer();
		MTextureManager* textureManager = renderer->getTextureManager();

		m_texture.texture = textureManager->acquireTexture("", m_textureDesc, data, false);
		if (m_texture.texture)
			m_texture.texture->textureDescription(m_textureDesc);

		// Flag as changed.
		m_textureChanged = true;

		return MStatus::kSuccess;
	}
	// Otherwise, update the existing texture.
	else
	{
		return m_texture.texture->update(data, false);
	}
}

// -----------------------------------------------------------------------------
const MTextureAssignment& FireRenderViewport::getTexture() const
{
	return m_texture;
}

// -----------------------------------------------------------------------------
bool FireRenderViewport::hasTextureChanged()
{
	bool changed = m_textureChanged;
	m_textureChanged = false;
	return changed;
}

// -----------------------------------------------------------------------------
void FireRenderViewport::addMenu()
{
	// The add menu command string.
	MString command;

	// Maya 2017 uses newer Python APIs, so create a
	// different command for the different Maya versions.
	if (MGlobal::apiVersion() >= 201700)
	{
		command =
			R"(from PySide2 import QtCore, QtWidgets, QtGui
import shiboken2
import maya.OpenMayaUI as omu
def setFireRenderAnimCache(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),cache=checked)
def clearFireRenderCache():
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),clear=True)
def setFireViewportMode_globalIllumination(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="globalIllumination")
def setFireViewportMode_directIllumination(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="directIllumination")
def setFireViewportMode_directIlluminationNoShadow(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="directIlluminationNoShadow")
def setFireViewportMode_wireframe(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="wireframe")
def setFireViewportMode_materialId(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="materialId")
def setFireViewportMode_position(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="position")
def setFireViewportMode_normal(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="normal")
def setFireViewportMode_texcoord(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="texcoord")
def setFireViewportMode_ambientOcclusion(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="ambientOcclusion")

ptr = omu.MQtUtil.findControl("m_panelName", long(omu.MQtUtil.mainWindow()))
w = shiboken2.wrapInstance(long(ptr), QtWidgets.QWidget)
menuBar = w.findChildren(QtWidgets.QMenuBar)[0]
frExist = False
for act in menuBar.actions():
	if act.text() == "FIRE_RENDER_NAME":
		frExist = True
if not frExist:
	frMenu = menuBar.addMenu("FIRE_RENDER_NAME")
	animAction = frMenu.addAction("Animation cache")
	animAction.setCheckable(True)
	animAction.setChecked(True)
	animAction.toggled.connect(setFireRenderAnimCache)
	action = frMenu.addAction("Clear animation cache")
	action.triggered.connect(clearFireRenderCache)


	frSubMenu = frMenu.addMenu("Viewport Mode")
	ag = QtWidgets.QActionGroup(frSubMenu)
	action = frSubMenu.addAction("globalIllumination")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.setChecked(True)
	action.triggered.connect(setFireViewportMode_globalIllumination)

	action = frSubMenu.addAction("directIllumination")
	action.setCheckable(True)
	action.setActionGroup(ag)
	action.triggered.connect(setFireViewportMode_directIllumination)

	action = frSubMenu.addAction("directIlluminationNoShadow")
	action.setCheckable(True)
	action.setActionGroup(ag)
	action.triggered.connect(setFireViewportMode_directIlluminationNoShadow)

	action = frSubMenu.addAction("wireframe")
	action.setCheckable(True)
	action.setActionGroup(ag)
	action.triggered.connect(setFireViewportMode_wireframe)

	action = frSubMenu.addAction("materialId")
	action.setCheckable(True)
	action.setActionGroup(ag)
	action.triggered.connect(setFireViewportMode_materialId)

	action = frSubMenu.addAction("position")
	action.setCheckable(True)
	action.setActionGroup(ag)
	action.triggered.connect(setFireViewportMode_position)

	action = frSubMenu.addAction("normal")
	action.setCheckable(True)
	action.setActionGroup(ag)
	action.triggered.connect(setFireViewportMode_normal)

	action = frSubMenu.addAction("texcoord")
	action.setCheckable(True)
	action.setActionGroup(ag)
	action.triggered.connect(setFireViewportMode_texcoord)

	action = frSubMenu.addAction("ambientOcclusion")
	action.setCheckable(True)
	action.setActionGroup(ag)
	action.triggered.connect(setFireViewportMode_ambientOcclusion)
)";
	}
	else
	{
		command =
			R"(from PySide import QtCore, QtGui
import shiboken
import maya.OpenMayaUI as omu
def setFireRenderAnimCache(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),cache=checked)
def clearFireRenderCache():
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),clear=True)
def setFireViewportMode_globalIllumination(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="globalIllumination")
def setFireViewportMode_directIllumination(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="directIllumination")
def setFireViewportMode_directIlluminationNoShadow(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="directIlluminationNoShadow")
def setFireViewportMode_wireframe(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="wireframe")
def setFireViewportMode_materialId(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="materialId")
def setFireViewportMode_position(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="position")
def setFireViewportMode_normal(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="normal")
def setFireViewportMode_texcoord(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="texcoord")
def setFireViewportMode_ambientOcclusion(checked=True):
	maya.cmds.fireRenderViewport(panel=maya.cmds.getPanel(wf=1),viewportMode="ambientOcclusion")

ptr = omu.MQtUtil.findControl("m_panelName", long(omu.MQtUtil.mainWindow()))
w = shiboken.wrapInstance(long(ptr), QtGui.QWidget)
menuBar = w.findChildren(QtGui.QMenuBar)[0]
frExist = False
for act in menuBar.actions():
	if act.text() == "FIRE_RENDER_NAME":
		frExist = True
if not frExist:
	frMenu = menuBar.addMenu("FIRE_RENDER_NAME")
	animAction = frMenu.addAction("Animation cache")
	animAction.setCheckable(True)
	animAction.setChecked(True)
	animAction.toggled.connect(setFireRenderAnimCache)
	action = frMenu.addAction("Clear animation cache")
	action.triggered.connect(clearFireRenderCache)

	frSubMenu = frMenu.addMenu("Viewport Mode")
	ag = QtGui.QActionGroup(frSubMenu)
	action = frSubMenu.addAction("globalIllumination")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.setChecked(True)
	action.triggered.connect(setFireViewportMode_globalIllumination)

	action = frSubMenu.addAction("directIllumination")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.triggered.connect(setFireViewportMode_directIllumination)

	action = frSubMenu.addAction("directIlluminationNoShadow")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.triggered.connect(setFireViewportMode_directIlluminationNoShadow)

	action = frSubMenu.addAction("wireframe")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.triggered.connect(setFireViewportMode_wireframe)

	action = frSubMenu.addAction("materialId")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.triggered.connect(setFireViewportMode_materialId)

	action = frSubMenu.addAction("position")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.triggered.connect(setFireViewportMode_position)

	action = frSubMenu.addAction("normal")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.triggered.connect(setFireViewportMode_normal)

	action = frSubMenu.addAction("texcoord")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.triggered.connect(setFireViewportMode_texcoord)

	action = frSubMenu.addAction("ambientOcclusion")
	action.setActionGroup(ag)
	action.setCheckable(True)
	action.triggered.connect(setFireViewportMode_ambientOcclusion)
)";
	}

	command.substitute("m_panelName", m_panelName);
	command.substitute("FIRE_RENDER_NAME", FIRE_RENDER_NAME);

	MGlobal::executePythonCommand(command);
}

// -----------------------------------------------------------------------------
void FireRenderViewport::removeMenu()
{
	// The remove menu command string.
	MString command;

	// Maya 2017 uses newer Python APIs, so create a
	// different command for the different Maya versions.
	if (MGlobal::apiVersion() >= 201700)
	{
		command =
			"from PySide2 import QtCore, QtWidgets\n"
			"import shiboken2\n"
			"import maya.OpenMayaUI as omu\n"
			"ptr = omu.MQtUtil.findControl(\"" + m_panelName + "\", long(omu.MQtUtil.mainWindow()))\n"
			"w = shiboken2.wrapInstance(long(ptr), QtWidgets.QWidget)\n"
			"menuBar = w.findChildren(QtWidgets.QMenuBar)[0]\n"
			"frExist = False\n"
			"for act in menuBar.actions():\n"
			"\tif act.text() == \"" + FIRE_RENDER_NAME + "\":\n"
			"\t\tmenuBar.removeAction(act)\n";
	}
	else
	{
		command =
			"from PySide import QtCore, QtGui\n"
			"import shiboken\n"
			"import maya.OpenMayaUI as omu\n"
			"ptr = omu.MQtUtil.findControl(\"" + m_panelName + "\", long(omu.MQtUtil.mainWindow()))\n"
			"w = shiboken.wrapInstance(long(ptr), QtGui.QWidget)\n"
			"menuBar = w.findChildren(QtGui.QMenuBar)[0]\n"
			"frExist = False\n"
			"for act in menuBar.actions():\n"
			"\tif act.text() == \"" + FIRE_RENDER_NAME + "\":\n"
			"\t\tmenuBar.removeAction(act)\n";
	}

	MGlobal::executePythonCommand(command);
}