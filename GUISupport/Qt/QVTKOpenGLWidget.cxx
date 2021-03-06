/*=========================================================================

  Program:   Visualization Toolkit
  Module:    QVTKOpenGLWidget.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "QVTKOpenGLWidget.h"

#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>

#include "QVTKInteractor.h"
#include "QVTKInteractorAdapter.h"
#include "vtkCommand.h"
#include "vtkGenericOpenGLRenderWindow.h"
#include "vtkImageData.h"
#include "vtkInteractorStyleTrackballCamera.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkOpenGLBufferObject.h"
#include "vtkOpenGLShaderCache.h"
#include "vtkOpenGLVertexArrayObject.h"
#include "vtkPointData.h"
#include "vtkRenderer.h"
#include "vtkRendererCollection.h"
#include "vtkShaderProgram.h"
#include "vtkUnsignedCharArray.h"

// #define DEBUG_QVTKOPENGL_WIDGET
#ifdef DEBUG_QVTKOPENGL_WIDGET
#define vtkQVTKOpenGLWidgetDebugMacro(msg) cout << this << ": " msg << endl;
#else
#define vtkQVTKOpenGLWidgetDebugMacro(x)
#endif

class QVTKOpenGLWidgetObserver : public vtkCommand
{
public:
  static QVTKOpenGLWidgetObserver* New() { return new QVTKOpenGLWidgetObserver(); }
  vtkBaseTypeMacro(QVTKOpenGLWidgetObserver, vtkCommand);

  void SetTarget(QVTKOpenGLWidget* target) { this->Target = target; }

  void Execute(vtkObject*, unsigned long eventId, void* callData) VTK_OVERRIDE
  {
    if (this->Target)
    {
      switch (eventId)
      {
        case vtkCommand::WindowMakeCurrentEvent:
          {
            // We do not call QOpenGLWidget::makeCurrent() as that also makes the
            // frame buffer object used by QOpenGLWidget active. This can have
            // unintended side effects when MakeCurrent gets called in a
            // render-pass, for example. We should only be making the context
            // active. To do that, we use this trick. We rely on the
            // QOpenGLContext have been called makeCurrent() previously so we
            // can get to the surface that was used to do that. We simply
            // reactivate on that surface.
            QOpenGLContext* ctxt = this->Target->context();
            QSurface* surface = ctxt? ctxt->surface() : NULL;
            if (surface)
            {
              ctxt->makeCurrent(surface);
            }
            Q_ASSERT(ctxt == NULL || surface != NULL);
          }
          break;

        case vtkCommand::WindowIsCurrentEvent:
        {
          bool& cstatus = *reinterpret_cast<bool*>(callData);
          cstatus = (QOpenGLContext::currentContext() == this->Target->context());
        }
        break;

        case vtkCommand::WindowFrameEvent:
          vtkQVTKOpenGLWidgetDebugMacro("frame");
          this->Target->windowFrameEventCallback();
          break;
      }
    }
  }

protected:
  QVTKOpenGLWidgetObserver() {}
  ~QVTKOpenGLWidgetObserver() {}
  QPointer<QVTKOpenGLWidget> Target;
};

//-----------------------------------------------------------------------------
QVTKOpenGLWidget::QVTKOpenGLWidget(QWidget* parentWdg, Qt::WindowFlags f)
  : Superclass(parentWdg, f)
  , AutomaticImageCacheEnabled(false)
  , MaxRenderRateForImageCache(1.0)
  , DeferRenderInPaintEvent(false)
  , InteractorAdaptor(NULL)
  , InPaintGL(false)
  , NeedToReinitializeWindow(false)
  , SkipRenderInPaintGL(false)
{
  this->Observer->SetTarget(this);

  // default to strong focus
  this->setFocusPolicy(Qt::StrongFocus);

  this->setAttribute(Qt::WA_NoSystemBackground, false);

  // Currently, we rely on QOpenGLWidget::PartialUpdate. This allows to easily
  // handle the case where vtkRenderWindow::Render() is called from outside
  // QVTKOpenGLWidget::paintGL(). When that happens, we simply skip calling
  // vtkRenderWindow::Render() when `painGL` gets called.
  this->setUpdateBehavior(QOpenGLWidget::PartialUpdate);

  this->InteractorAdaptor = new QVTKInteractorAdapter(this);
  this->InteractorAdaptor->SetDevicePixelRatio(this->devicePixelRatio());

  this->setMouseTracking(true);

  this->DeferedRenderTimer.setSingleShot(true);
  this->DeferedRenderTimer.setInterval(0);
  this->connect(&this->DeferedRenderTimer, SIGNAL(timeout()), SLOT(doDeferredRender()));
}

//-----------------------------------------------------------------------------
QVTKOpenGLWidget::~QVTKOpenGLWidget()
{
  this->makeCurrent();
  this->SetRenderWindow(static_cast<vtkGenericOpenGLRenderWindow*>(NULL));
  this->Observer->SetTarget(NULL);
  delete this->InteractorAdaptor;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::SetRenderWindow(vtkRenderWindow* win)
{
  this->SetRenderWindow(vtkGenericOpenGLRenderWindow::SafeDownCast(win));
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::SetRenderWindow(vtkGenericOpenGLRenderWindow* win)
{
  if (this->RenderWindow == win)
  {
    return;
  }

  if (this->RenderWindow)
  {
    this->RenderWindow->RemoveObserver(this->Observer.Get());
  }

  // before releasing old render window, release any textures we may have
  // allocated on it.
  this->markCachedImageAsDirty();

  this->InteractorAdaptor->SetDevicePixelRatio(this->devicePixelRatio());

  this->RenderWindow = win;
  if (this->RenderWindow)
  {
    // tell the vtk window what the size of this window is
    this->RenderWindow->SetReadyForRendering(false);
    this->RenderWindow->SetSize(this->width() * this->devicePixelRatio(),
                                this->height() * this->devicePixelRatio());
    this->RenderWindow->SetPosition(this->x() * this->devicePixelRatio(),
                                    this->y() * this->devicePixelRatio());

    // if an interactor wasn't provided, we'll make one by default
    if (!this->RenderWindow->GetInteractor())
    {
      // create a default interactor
      vtkNew<QVTKInteractor> iren;
      // iren->SetUseTDx(this->UseTDx);
      this->RenderWindow->SetInteractor(iren.Get());
      iren->Initialize();

      // now set the default style
      vtkNew<vtkInteractorStyleTrackballCamera> style;
      iren->SetInteractorStyle(style.Get());
    }

    // tell the interactor the size of this window
    this->RenderWindow->GetInteractor()
        ->SetSize(this->width() * this->devicePixelRatio(),
                  this->height() * this->devicePixelRatio());

    // Add an observer to monitor when the image changes.  Should work most
    // of the time.  The application will have to call
    // markCachedImageAsDirty for any other case.
    this->RenderWindow->AddObserver(vtkCommand::WindowMakeCurrentEvent, this->Observer.Get());
    this->RenderWindow->AddObserver(vtkCommand::WindowIsCurrentEvent, this->Observer.Get());
    this->RenderWindow->AddObserver(vtkCommand::WindowFrameEvent, this->Observer.Get());
    this->NeedToReinitializeWindow = true;
  }
}

//-----------------------------------------------------------------------------
vtkRenderWindow* QVTKOpenGLWidget::GetRenderWindow()
{
  return this->RenderWindow.Get();
}

//-----------------------------------------------------------------------------
QVTKInteractor* QVTKOpenGLWidget::GetInteractor()
{
  return QVTKInteractor::SafeDownCast(this->GetRenderWindow()->GetInteractor());
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::markCachedImageAsDirty()
{
  // this is done as an extra precaution.
  this->SkipRenderInPaintGL = false;
  if (this->CachedTexture)
  {
    this->CachedTexture = NULL;
    emit this->cachedImageDirty();
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::saveImageToCache()
{
  // this only works in non-multisampling mode.
  if (this->RenderWindow->GetMultiSamples())
  {
    return;
  }

  if (this->CachedTexture == NULL)
  {
    this->CachedTexture = vtkSmartPointer<vtkTextureObject>::New();
    this->CachedTexture->SetWrapS(vtkTextureObject::ClampToEdge);
    this->CachedTexture->SetWrapT(vtkTextureObject::ClampToEdge);
    this->CachedTexture->SetMagnificationFilter(vtkTextureObject::Nearest);
    this->CachedTexture->SetMinificationFilter(vtkTextureObject::Nearest);
  }
  this->CachedTexture->SetContext(vtkOpenGLRenderWindow::SafeDownCast(this->RenderWindow));
  const int* wsize = this->RenderWindow->GetSize();
  this->CachedTexture->Allocate2D(wsize[0], wsize[1], 3, VTK_UNSIGNED_CHAR);
  this->CachedTexture->CopyFromFrameBuffer(0, 0, 0, 0, wsize[0], wsize[1]);
  this->CachedTexture->Deactivate();
  emit this->cachedImageClean();
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::setAutomaticImageCacheEnabled(bool flag)
{
  if (this->AutomaticImageCacheEnabled != flag)
  {
    this->AutomaticImageCacheEnabled = flag;
    if (!flag)
    {
      this->markCachedImageAsDirty();
    }
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::setMaxRenderRateForImageCache(double rate)
{
  this->MaxRenderRateForImageCache = rate;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::setDeferRenderInPaintEvent(bool val)
{
  this->DeferRenderInPaintEvent = val;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::copyFromFormat(const QSurfaceFormat& format, vtkRenderWindow* win)
{
  if (vtkOpenGLRenderWindow* oglWin = vtkOpenGLRenderWindow::SafeDownCast(win))
  {
    oglWin->SetStereoCapableWindow(format.stereo() ? 1 : 0);
    oglWin->SetMultiSamples(format.samples());
    oglWin->SetStencilCapable(format.stencilBufferSize() > 0);
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::copyToFormat(vtkRenderWindow* win, QSurfaceFormat& format)
{
  if (vtkOpenGLRenderWindow* oglWin = vtkOpenGLRenderWindow::SafeDownCast(win))
  {
    format.setStereo(oglWin->GetStereoCapableWindow());
    format.setSamples(oglWin->GetMultiSamples());
    format.setStencilBufferSize(oglWin->GetStencilCapable() ? 8 : 0);
  }
}

//-----------------------------------------------------------------------------
QSurfaceFormat QVTKOpenGLWidget::defaultFormat()
{
  QSurfaceFormat fmt;
  fmt.setRenderableType(QSurfaceFormat::OpenGL);
  fmt.setVersion(3, 2);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
  fmt.setRedBufferSize(1);
  fmt.setGreenBufferSize(1);
  fmt.setBlueBufferSize(1);
  fmt.setDepthBufferSize(1);
  fmt.setStencilBufferSize(0);
  fmt.setAlphaBufferSize(0);
  fmt.setStereo(false);
  fmt.setSamples(vtkOpenGLRenderWindow::GetGlobalMaximumNumberOfMultiSamples());
  return fmt;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::initializeGL()
{
  vtkQVTKOpenGLWidgetDebugMacro("initializeGL");
  this->Superclass::initializeGL();
  if (this->RenderWindow)
  {
    // use QSurfaceFormat for the widget, update ivars on the vtkRenderWindow.
    QVTKOpenGLWidget::copyFromFormat(this->format(), this->RenderWindow);
  }

  this->connect(
    this->context(), SIGNAL(aboutToBeDestroyed()), SLOT(cleanupContext()), Qt::UniqueConnection);
  this->NeedToReinitializeWindow = true;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::resizeGL(int w, int h)
{
  vtkQVTKOpenGLWidgetDebugMacro("resizeGL");
  vtkRenderWindowInteractor* iren = this->RenderWindow ? this->RenderWindow->GetInteractor() : nullptr;
  this->InteractorAdaptor->SetDevicePixelRatio(this->devicePixelRatio(), iren);
  if (this->RenderWindow)
  {
    this->RenderWindow->SetSize(w * this->devicePixelRatio(),
                                h * this->devicePixelRatio());
    this->markCachedImageAsDirty();
  }
  this->Superclass::resizeGL(w, h);
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::paintGL()
{
  if (this->SkipRenderInPaintGL)
  {
    vtkQVTKOpenGLWidgetDebugMacro("paintGL:skipped");
    this->SkipRenderInPaintGL = false;
    return;
  }

  vtkQVTKOpenGLWidgetDebugMacro("paintGL");
  bool prev = this->InPaintGL;
  this->InPaintGL = true;
  this->Superclass::paintGL();
  if (this->NeedToReinitializeWindow && this->RenderWindow)
  {
    this->RenderWindow->SetForceMaximumHardwareLineWidth(1);
    this->RenderWindow->SetReadyForRendering(true);
    this->RenderWindow->InitializeFromCurrentContext();
    this->NeedToReinitializeWindow = false;
  }

  // if we have a saved image, use it
  if (this->paintCachedImage() == false)
  {
    if (this->DeferRenderInPaintEvent)
    {
      this->deferRender();
    }
    else
    {
      this->doDeferredRender();
    }
  }
  this->InPaintGL = prev;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::cleanupContext()
{
  // QOpenGLWidget says when this slot is called, the context may not be current
  // and hence is a good practice to make it so.
  this->makeCurrent();

  vtkQVTKOpenGLWidgetDebugMacro("cleanupContext");
  if (this->RenderWindow)
  {
    this->RenderWindow->Finalize();
    this->RenderWindow->SetReadyForRendering(false);
  }
  this->markCachedImageAsDirty();
}

//-----------------------------------------------------------------------------
bool QVTKOpenGLWidget::paintCachedImage()
{
  // if we have a saved image, use it
  if (this->CachedTexture)
  {
    vtkQVTKOpenGLWidgetDebugMacro("using cache");
    // Since we're not caching depth buffer (should we), we clear depth buffer
    // before pasting the cached image.
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    f->glClear(GL_DEPTH_BUFFER_BIT);
    this->CachedTexture->CopyToFrameBuffer(0, 0, this->CachedTexture->GetWidth() - 1,
      this->CachedTexture->GetHeight() - 1, 0, 0, this->width() - 1, this->height() - 1, NULL,
      NULL);
    this->CachedTexture->Deactivate();
    return true;
  }
  return false;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::windowFrameEventCallback()
{
  Q_ASSERT(this->RenderWindow);

  if (!this->InPaintGL)
  {
    // Handing vtkOpenGLRenderWindow::Frame is tricky. VTK code traditionally
    // calls `Frame` to indicate that VTK is done rendering 1 frame. Now, when
    // that happens, should we tell Qt to update the widget -- that's the
    // question? In general, yes, but sometimes VTK does things in the
    // background i.e. back buffer without wanting to update the front buffer
    // e.g. when making selections. In that case, we don't want to update Qt
    // widget either, since whatever it did was not meant to be visible.
    // To handle that, we check if vtkOpenGLRenderWindow::SwapBuffers is true,
    // and request an update only when it is.
    if (this->RenderWindow->GetSwapBuffers())
    {
      this->SkipRenderInPaintGL = true;

      // Means that the vtkRenderWindow rendered outside a paintGL call. That can
      // happen when application code call vtkRenderWindow::Render() directly,
      // instead of calling QVTKOpenGLWidget::update() or letting Qt update the
      // widget. In that case, since QOpenGLWidget rendering into an offscreen
      // FBO, the result still needs to be composed by Qt widget stack. We request
      // that using `update()`.
      vtkQVTKOpenGLWidgetDebugMacro("update");
      this->update();
    }
    else
    {
      // VTK has destroyed the frame buffer by rendering something in it. Now,
      // if Qt for some reason comes back with a paint request, then we'll need
      // to ensure VTK renders again (or uses cache, if caching was enabled).
      this->SkipRenderInPaintGL = false;
    }
  }

  // prevent capturing the selection buffer as the cached image. to do this
  // we iterate through each renderer in the view and check if they have an
  // active selector object. if so we return without saving the image
  vtkRendererCollection* renderers = this->RenderWindow->GetRenderers();
  if (renderers)
  {
    renderers->InitTraversal();
    while (vtkRenderer* renderer = renderers->GetNextItem())
    {
      if (renderer->GetSelector() != NULL)
      {
        return;
      }
    }
  }

  // Render happened. If we have requested a render to happen, it has happened,
  // so no need to request another render. Stop the timer.
  this->DeferedRenderTimer.stop();
  if (this->isAutomaticImageCacheEnabled() &&
    (this->RenderWindow->GetDesiredUpdateRate() < this->maxRenderRateForImageCache()) &&
    this->RenderWindow->GetSwapBuffers())
  {
    this->saveImageToCache();
    vtkQVTKOpenGLWidgetDebugMacro("saving to cache");
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::deferRender()
{
  this->DeferedRenderTimer.start();
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::doDeferredRender()
{
  vtkRenderWindowInteractor* iren = this->RenderWindow ? this->RenderWindow->GetInteractor() : NULL;
  if (iren)
  {
    iren->Render();
    this->DeferedRenderTimer.stop(); // not necessary, but no harm.
  }
}

//-----------------------------------------------------------------------------
bool QVTKOpenGLWidget::event(QEvent* evt)
{
  switch (evt->type())
  {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
      // skip events that are explicitly handled by overrides to avoid duplicate
      // calls to InteractorAdaptor->ProcessEvent().
      break;

    default:
      if (this->RenderWindow && this->RenderWindow->GetInteractor())
      {
        this->InteractorAdaptor->ProcessEvent(evt, this->RenderWindow->GetInteractor());
      }
  }
  return this->Superclass::event(evt);
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::moveEvent(QMoveEvent* evt)
{
  this->Superclass::moveEvent(evt);
  if (this->RenderWindow)
  {
    this->RenderWindow->SetPosition(this->x(), this->y());
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::mousePressEvent(QMouseEvent* event)
{
  emit mouseEvent(event);

  if (this->RenderWindow && this->RenderWindow->GetInteractor())
  {
    this->InteractorAdaptor->ProcessEvent(event,
                                          this->RenderWindow->GetInteractor());
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::mouseMoveEvent(QMouseEvent* event)
{
  emit mouseEvent(event);

  if (this->RenderWindow && this->RenderWindow->GetInteractor())
  {
    this->InteractorAdaptor->ProcessEvent(event,
                                          this->RenderWindow->GetInteractor());
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::mouseReleaseEvent(QMouseEvent* event)
{
  emit mouseEvent(event);

  if (this->RenderWindow && this->RenderWindow->GetInteractor())
  {
    this->InteractorAdaptor->ProcessEvent(event,
                                          this->RenderWindow->GetInteractor());
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
  emit mouseEvent(event);

  if (this->RenderWindow && this->RenderWindow->GetInteractor())
  {
    this->InteractorAdaptor->ProcessEvent(event,
                                          this->RenderWindow->GetInteractor());
  }
}
