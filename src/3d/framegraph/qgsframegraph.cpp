/***************************************************************************
  qgsframegraph.cpp
  --------------------------------------
  Date                 : August 2020
  Copyright            : (C) 2020 by Belgacem Nedjima
  Email                : gb underscore nedjima at esi dot dz
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsframegraph.h"
#include "moc_qgsframegraph.cpp"
#include "qgsdirectionallightsettings.h"
#include "qgspostprocessingentity.h"
#include "qgs3dutils.h"
#include "qgsframegraphutils.h"
#include "qgsabstractrenderview.h"
#include "qgsshadowrenderview.h"


#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
#include <Qt3DRender/QAttribute>
#include <Qt3DRender/QBuffer>
#include <Qt3DRender/QGeometry>

typedef Qt3DRender::QAttribute Qt3DQAttribute;
typedef Qt3DRender::QBuffer Qt3DQBuffer;
typedef Qt3DRender::QGeometry Qt3DQGeometry;
#else
#include <Qt3DCore/QAttribute>
#include <Qt3DCore/QBuffer>
#include <Qt3DCore/QGeometry>

typedef Qt3DCore::QAttribute Qt3DQAttribute;
typedef Qt3DCore::QBuffer Qt3DQBuffer;
typedef Qt3DCore::QGeometry Qt3DQGeometry;
#endif

#include <Qt3DRender/QGeometryRenderer>
#include <Qt3DRender/QTechnique>
#include <Qt3DRender/QGraphicsApiFilter>
#include <Qt3DRender/QBlendEquation>
#include <Qt3DRender/QColorMask>
#include <Qt3DRender/QSortPolicy>
#include <Qt3DRender/QNoDepthMask>
#include <Qt3DRender/QBlendEquationArguments>
#include <Qt3DRender/QAbstractTexture>
#include <Qt3DRender/QNoDraw>
#include <Qt3DRender/QNoPicking>
#include "qgsshadowrenderview.h"
#include "qgsforwardrenderview.h"
#include "qgsdepthrenderview.h"
#include "qgsdepthentity.h"
#include "qgsdebugtexturerenderview.h"
#include "qgsdebugtextureentity.h"
#include "qgsambientocclusionrenderview.h"

const QString QgsFrameGraph::FORWARD_RENDERVIEW = "forward";
const QString QgsFrameGraph::SHADOW_RENDERVIEW = "shadow";
const QString QgsFrameGraph::AXIS3D_RENDERVIEW = "3daxis";
const QString QgsFrameGraph::DEPTH_RENDERVIEW = "depth";
const QString QgsFrameGraph::DEBUG_RENDERVIEW = "debug_texture";
const QString QgsFrameGraph::AMBIENT_OCCLUSION_RENDERVIEW = "ambient_occlusion";

void QgsFrameGraph::constructForwardRenderPass(EyeTarget eye)
{
  registerRenderView( std::make_unique<QgsForwardRenderView>( FORWARD_RENDERVIEW, mainCamera( eye ), mRenderLayer, mTransparentObjectsLayer ), FORWARD_RENDERVIEW, eye );
}

void QgsFrameGraph::constructShadowRenderPass(EyeTarget eye)
{
  registerRenderView( std::make_unique<QgsShadowRenderView>( SHADOW_RENDERVIEW, mEntityCastingShadowsLayer ), SHADOW_RENDERVIEW, eye );
}

void QgsFrameGraph::constructDebugTexturePass(EyeTarget eye, Qt3DRender::QFrameGraphNode *topNode )
{
  registerRenderView( std::make_unique<QgsDebugTextureRenderView>( DEBUG_RENDERVIEW ), DEBUG_RENDERVIEW, eye, topNode );
}

Qt3DRender::QFrameGraphNode *QgsFrameGraph::constructSubPostPassForProcessing(EyeTarget eye)
{
  Qt3DRender::QCameraSelector *cameraSelector = new Qt3DRender::QCameraSelector;
  cameraSelector->setObjectName( "Sub pass Postprocessing" );
  cameraSelector->setCamera( shadowRenderView(eye).lightCamera() );

  Qt3DRender::QLayerFilter *layerFilter = new Qt3DRender::QLayerFilter( cameraSelector );

  // could be the first of this branch
  new Qt3DRender::QClearBuffers( layerFilter );

  auto &renderData = eye == EyeTarget::Left ? mLeftRenderData : mRightRenderData;
  Qt3DRender::QLayer *postProcessingLayer = new Qt3DRender::QLayer();
  renderData.mPostprocessingEntity = new QgsPostprocessingEntity( this, eye, postProcessingLayer, mRootEntity );
  layerFilter->addLayer( postProcessingLayer );
  renderData.mPostprocessingEntity->setObjectName( "PostProcessingPassEntity" );

  return cameraSelector;
}

Qt3DRender::QFrameGraphNode *QgsFrameGraph::constructSubPostPassForRenderCapture(EyeTarget eye)
{
  Qt3DRender::QFrameGraphNode *top = new Qt3DRender::QNoDraw;
  top->setObjectName( "Sub pass RenderCapture" );

  auto &captureData = eye == EyeTarget::Left ? mLeftRenderData : mRightRenderData;
  captureData.mRenderCapture = new Qt3DRender::QRenderCapture( top );

  return top;
}

Qt3DRender::QFrameGraphNode *QgsFrameGraph::constructPostprocessingPass(EyeTarget eye)
{
  auto &captureData = eye == EyeTarget::Left ? mLeftRenderData : mRightRenderData;
  captureData.mSelector = new Qt3DRender::QRenderTargetSelector;
  captureData.mSelector->setObjectName( "Postprocessing render pass" );
  captureData.mSelector->setEnabled( mRenderCaptureEnabled );

  Qt3DRender::QRenderTarget *renderTarget = new Qt3DRender::QRenderTarget( captureData.mSelector );

  // The lifetime of the objects created here is managed
  // automatically, as they become children of this object.

  // Create a render target output for rendering color.
  Qt3DRender::QRenderTargetOutput *colorOutput = new Qt3DRender::QRenderTargetOutput( renderTarget );
  colorOutput->setAttachmentPoint( Qt3DRender::QRenderTargetOutput::Color0 );

  // Create a texture to render into.
  captureData.mColorCaptureTexture = new Qt3DRender::QTexture2D( colorOutput );
  captureData.mColorCaptureTexture->setSize( mSize.width(), mSize.height() );
  captureData.mColorCaptureTexture->setFormat( Qt3DRender::QAbstractTexture::RGB8_UNorm );
  captureData.mColorCaptureTexture->setMinificationFilter( Qt3DRender::QAbstractTexture::Linear );
  captureData.mColorCaptureTexture->setMagnificationFilter( Qt3DRender::QAbstractTexture::Linear );
  captureData.mColorCaptureTexture->setObjectName( "PostProcessingPass::ColorTarget" );

  // Hook the texture up to our output, and the output up to this object.
  colorOutput->setTexture( captureData.mColorCaptureTexture );
  renderTarget->addOutput( colorOutput );

  Qt3DRender::QRenderTargetOutput *depthOutput = new Qt3DRender::QRenderTargetOutput( renderTarget );

  depthOutput->setAttachmentPoint( Qt3DRender::QRenderTargetOutput::Depth );
  captureData.mDepthCaptureTexture = new Qt3DRender::QTexture2D( depthOutput );
  captureData.mDepthCaptureTexture->setSize( mSize.width(), mSize.height() );
  captureData.mDepthCaptureTexture->setFormat( Qt3DRender::QAbstractTexture::DepthFormat );
  captureData.mDepthCaptureTexture->setMinificationFilter( Qt3DRender::QAbstractTexture::Linear );
  captureData.mDepthCaptureTexture->setMagnificationFilter( Qt3DRender::QAbstractTexture::Linear );
  captureData.mDepthCaptureTexture->setComparisonFunction( Qt3DRender::QAbstractTexture::CompareLessEqual );
  captureData.mDepthCaptureTexture->setComparisonMode( Qt3DRender::QAbstractTexture::CompareRefToTexture );
  captureData.mDepthCaptureTexture->setObjectName( "PostProcessingPass::DepthTarget" );

  depthOutput->setTexture( captureData.mDepthCaptureTexture );
  renderTarget->addOutput( depthOutput );

  captureData.mSelector->setTarget( renderTarget );

  // sub passes:
  constructSubPostPassForProcessing(eye)->setParent( captureData.mSelector );
  constructDebugTexturePass( eye, captureData.mSelector );
  constructSubPostPassForRenderCapture(eye)->setParent( captureData.mSelector );

  return captureData.mSelector;
}

void QgsFrameGraph::constructAmbientOcclusionRenderPass(EyeTarget eye)
{
  Qt3DRender::QTexture2D *forwardDepthTexture = forwardRenderView(eye).depthTexture();

  QgsAmbientOcclusionRenderView *aorv = new QgsAmbientOcclusionRenderView( AMBIENT_OCCLUSION_RENDERVIEW, mMainCamera, mSize, forwardDepthTexture, mRootEntity );
  registerRenderView( std::unique_ptr<QgsAmbientOcclusionRenderView>( aorv ), AMBIENT_OCCLUSION_RENDERVIEW, eye );
}


Qt3DRender::QFrameGraphNode *QgsFrameGraph::constructFocusPlanePass(EyeTarget eye)
{
  auto *cursorCameraSelector = new Qt3DRender::QCameraSelector;
  cursorCameraSelector->setCamera( mainCamera( eye ) );

  auto *cursorLayerFilter = new Qt3DRender::QLayerFilter( cursorCameraSelector );
  cursorLayerFilter->addLayer( mCursorLayer );

  auto *cursorStateSet = new Qt3DRender::QRenderStateSet( cursorLayerFilter );
  Qt3DRender::QDepthTest *depthTest = new Qt3DRender::QDepthTest;
  depthTest->setDepthFunction( Qt3DRender::QDepthTest::Always );
  auto *noDepthWrite = new Qt3DRender::QNoDepthMask;
  cursorStateSet->addRenderState( depthTest );
  cursorStateSet->addRenderState( noDepthWrite );

  // Here we attach our drawings to the render target also used by forward pass.
  // This is kind of okay, but as a result, post-processing effects get applied
  // to rubber bands too. Ideally we would want them on top of everything.
  auto *cursorRenderTargetSelector = new Qt3DRender::QRenderTargetSelector( cursorStateSet );
  QgsForwardRenderView *frv = dynamic_cast<QgsForwardRenderView *>( renderView( FORWARD_RENDERVIEW, eye ) );
  cursorRenderTargetSelector->setTarget( frv->renderTargetSelector()->target() );

  return cursorCameraSelector;
}

Qt3DRender::QFrameGraphNode *QgsFrameGraph::constructCursorPass(EyeTarget eye)
{
  auto *focusPlaneCameraSelector = new Qt3DRender::QCameraSelector;
  focusPlaneCameraSelector->setCamera( mainCamera( eye ) );

  auto *focusPlaneLayerFilter = new Qt3DRender::QLayerFilter( focusPlaneCameraSelector );
  focusPlaneLayerFilter->addLayer( mFocusPlaneLayer );

  auto *focusPlaneStateSet = new Qt3DRender::QRenderStateSet( focusPlaneLayerFilter );
  Qt3DRender::QDepthTest *depthTest = new Qt3DRender::QDepthTest;
  depthTest->setDepthFunction( Qt3DRender::QDepthTest::Less );
  auto *noDepthWrite = new Qt3DRender::QNoDepthMask;
  focusPlaneStateSet->addRenderState( depthTest );
  focusPlaneStateSet->addRenderState( noDepthWrite );

  // Here we attach our drawings to the render target also used by forward pass.
  // This is kind of okay, but as a result, post-processing effects get applied
  // to rubber bands too. Ideally we would want them on top of everything.
  auto *focusPlaneRenderTargetSelector = new Qt3DRender::QRenderTargetSelector( focusPlaneStateSet );
  QgsForwardRenderView *frv = dynamic_cast<QgsForwardRenderView *>( renderView( FORWARD_RENDERVIEW, eye ) );
  focusPlaneRenderTargetSelector->setTarget( frv->renderTargetSelector()->target() );

  return focusPlaneCameraSelector;
}

Qt3DRender::QFrameGraphNode *QgsFrameGraph::constructRubberBandsPass(EyeTarget eye)
{
  mRubberBandsCameraSelector = new Qt3DRender::QCameraSelector;
  mRubberBandsCameraSelector->setObjectName( "RubberBands Pass CameraSelector" );
  mRubberBandsCameraSelector->setCamera( mMainCamera );

  mRubberBandsLayerFilter = new Qt3DRender::QLayerFilter( mRubberBandsCameraSelector );
  mRubberBandsLayerFilter->addLayer( mRubberBandsLayer );

  Qt3DRender::QBlendEquationArguments *blendState = new Qt3DRender::QBlendEquationArguments;
  blendState->setSourceRgb( Qt3DRender::QBlendEquationArguments::SourceAlpha );
  blendState->setDestinationRgb( Qt3DRender::QBlendEquationArguments::OneMinusSourceAlpha );

  Qt3DRender::QBlendEquation *blendEquation = new Qt3DRender::QBlendEquation;
  blendEquation->setBlendFunction( Qt3DRender::QBlendEquation::Add );

  mRubberBandsStateSet = new Qt3DRender::QRenderStateSet( mRubberBandsLayerFilter );
  Qt3DRender::QDepthTest *depthTest = new Qt3DRender::QDepthTest;
  depthTest->setDepthFunction( Qt3DRender::QDepthTest::Always );
  mRubberBandsStateSet->addRenderState( depthTest );
  mRubberBandsStateSet->addRenderState( blendState );
  mRubberBandsStateSet->addRenderState( blendEquation );

  // Here we attach our drawings to the render target also used by forward pass.
  // This is kind of okay, but as a result, post-processing effects get applied
  // to rubber bands too. Ideally we would want them on top of everything.
  mRubberBandsRenderTargetSelector = new Qt3DRender::QRenderTargetSelector( mRubberBandsStateSet );
  mRubberBandsRenderTargetSelector->setTarget( forwardRenderView(eye).renderTargetSelector()->target() );

  return mRubberBandsCameraSelector;
}


void QgsFrameGraph::constructDepthRenderPass(EyeTarget eye)
{
  // entity used to draw the depth texture and convert it to rgb image
  Qt3DRender::QTexture2D *forwardDepthTexture = forwardRenderView(eye).depthTexture();
  QgsDepthRenderView *rv = new QgsDepthRenderView( DEPTH_RENDERVIEW, mSize, forwardDepthTexture, mRootEntity );
  registerRenderView( std::unique_ptr<QgsDepthRenderView>( rv ), DEPTH_RENDERVIEW, eye );
}

Qt3DRender::QRenderCapture *QgsFrameGraph::depthRenderCapture(EyeTarget eye)
{
  return depthRenderView(eye).renderCapture();
}

QgsFrameGraph::QgsFrameGraph(QSurface *surface, QSize s, Qt3DRender::QCamera *mainCamera, Qt3DRender::QCamera *rightCamera, Qt3DRender::QCamera *leftCamera, Qt3DCore::QEntity *root , bool enableStereo)
  : Qt3DCore::QEntity( root )
  , mSize( s )
  , mStereoEnabled(enableStereo)
{
  // general overview of how the frame graph looks:
  //
  //  +------------------------+    using window or
  //  | QRenderSurfaceSelector |   offscreen surface
  //  +------------------------+
  //             |
  //  +-----------+
  //  | QViewport | (0,0,1,1)
  //  +-----------+
  //             |
  //             +-------------------------
  //             |                        |
  //  +------------+                +------------------+
  //  | QNoPicking |                | picking / nodraw |
  //  +------------+                +------------------+
  //             | (one per eye)
  //     +--------------------------+-------------------+-----------------+
  //     |                          |                   |                 |
  // +--------------------+ +--------------+ +-----------------+ +-----------------+
  // | two forward passes | | shadows pass | |  depth buffer   | | post-processing |
  // |  (solid objects    | |              | | processing pass | |    passes       |
  // |  and transparent)  | +--------------+ +-----------------+ +-----------------+
  // +--------------------+
  //
  // Notes:
  // - depth buffer processing pass is used whenever we need depth map information
  //   (for camera navigation) and it converts depth texture to a color texture
  //   so that we can capture it with QRenderCapture - currently it is unable
  //   to capture depth buffer, only colors (see QTBUG-65155)
  // - there are multiple post-processing passes that take rendered output
  //   of the scene, optionally apply effects (add shadows, ambient occlusion,
  //   eye dome lighting) and finally output to the given surface
  // - there may be also two more passes when 3D axis is shown - see Qgs3DAxis

  mRootEntity = root;
  mMainCamera = mainCamera;
  mLeftRenderData.mCamera = leftCamera;
  mRightRenderData.mCamera = rightCamera;

  mRenderSurfaceSelector = new Qt3DRender::QRenderSurfaceSelector;

  QObject *surfaceObj = dynamic_cast<QObject *>( surface );
  Q_ASSERT( surfaceObj );

  mRenderSurfaceSelector->setSurface( surfaceObj );
  mRenderSurfaceSelector->setExternalRenderTargetSize( mSize );

  mMainViewPort = new Qt3DRender::QViewport( mRenderSurfaceSelector );
  mMainViewPort->setNormalizedRect( QRectF( 0.0f, 0.0f, 1.0f, 1.0f ) );

  mRenderLayer = new Qt3DRender::QLayer;
  mRenderLayer->setRecursive( true );
  mRenderLayer->setObjectName( "Render::Layer" );

  mTransparentObjectsLayer = new Qt3DRender::QLayer;
  mTransparentObjectsLayer->setRecursive( true );
  mTransparentObjectsLayer->setObjectName( "Render::TransparentLayer" );

  mEntityCastingShadowsLayer = new Qt3DRender::QLayer;
  mEntityCastingShadowsLayer->setRecursive( true );
  mEntityCastingShadowsLayer->setObjectName( "Render::CastingShadowsLayer" );

  // Other layers
  mRubberBandsLayer = new Qt3DRender::QLayer(this);
  mRubberBandsLayer->setObjectName( "mRubberBandsLayer" );
  mRubberBandsLayer->setRecursive( true );

  mCursorLayer = new Qt3DRender::QLayer(this);
  mCursorLayer->setObjectName( "mCursorLayer" );
  mCursorLayer->setRecursive( true );

  mFocusPlaneLayer = new Qt3DRender::QLayer(this);
  mFocusPlaneLayer->setObjectName( "mFocusPlaneLayer" );
  mFocusPlaneLayer->setRecursive( true );

  // Disable picking for rendering branches
  auto *noPicking = new Qt3DRender::QNoPicking(mMainViewPort);

  // Center/Left Eye Branch
  mLeftRenderData.mRoot = new Qt3DRender::QFrameGraphNode( noPicking );
  mLeftRenderData.mRoot->setObjectName( "LeftEye" );

  auto createSubGraphForStereo = [this] (Qt3DRender::QFrameGraphNode *root, EyeTarget eye ) {
    // Forward render
    constructForwardRenderPass(eye);

    Qt3DRender::QFrameGraphNode *focusPlanePass = constructFocusPlanePass(eye);
    focusPlanePass->setParent( root );
    focusPlanePass->setObjectName( "focusPlanePass" );

    // rubber bands (they should be always on top)
    Qt3DRender::QFrameGraphNode *rubberBandsPass = constructRubberBandsPass(eye);
    rubberBandsPass->setObjectName( "rubberBandsPass" );
    rubberBandsPass->setParent( mMainViewPort );

    // 3d cursor (should be always on top)
    Qt3DRender::QFrameGraphNode *cursorPass = constructCursorPass( eye );
    cursorPass->setParent( root );
    cursorPass->setObjectName( "cursorPass" );

    // shadow rendering pass
    constructShadowRenderPass(eye);

    // depth buffer processing
    constructDepthRenderPass(eye);

    // Ambient occlusion factor render pass
    constructAmbientOcclusionRenderPass(eye);

    // post process
    Qt3DRender::QFrameGraphNode *postprocessingPass = constructPostprocessingPass(eye);
    postprocessingPass->setParent( mMainViewPort );
    postprocessingPass->setObjectName( "PostProcessingPass" );
  };

  createSubGraphForStereo( mLeftRenderData.mRoot, EyeTarget::Left );
  if (mStereoEnabled)
  {
    // Right Eye Branch if stereo enabled
    mRightRenderData.mRoot = new Qt3DRender::QFrameGraphNode( noPicking );
    mRightRenderData.mRoot->setObjectName("RightEye");
    createSubGraphForStereo( mRightRenderData.mRoot, EyeTarget::Right );
  }

  // Picking Branch
  auto *noDraw = new Qt3DRender::QNoDraw(mMainViewPort);
  noDraw->setObjectName("Picking");
  auto *pickingCameraSelection = new Qt3DRender::QCameraSelector(noDraw);
  pickingCameraSelection->setCamera(mMainCamera);

  // Other renderable objects
  mRubberBandsRootEntity = new Qt3DCore::QEntity( mRootEntity );
  mRubberBandsRootEntity->setObjectName( "mRubberBandsRootEntity" );
  mRubberBandsRootEntity->addComponent( mRubberBandsLayer );

  mCursorRootEntity = new Qt3DCore::QEntity( mRootEntity );
  mCursorRootEntity->addComponent( mCursorLayer );
  mCursorRootEntity->setObjectName( "mCursorRootEntity" );

  mFocusPlaneRootEntity = new Qt3DCore::QEntity( mRootEntity );
  mFocusPlaneRootEntity->addComponent( mFocusPlaneLayer );
  mFocusPlaneRootEntity->setObjectName( "mFocusPlaneRootEntity" );
}

void QgsFrameGraph::unregisterRenderView( const QString &name, EyeTarget eye )
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  auto it = table.find( name );
  if ( it != table.end() )
  {
    (*it).second->topGraphNode()->setParent( ( QNode * ) nullptr );
    table.erase(it);
  }
}

bool QgsFrameGraph::registerRenderView(std::unique_ptr<QgsAbstractRenderView> renderView, const QString &name, EyeTarget eye, Qt3DRender::QFrameGraphNode *topNode )
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  bool out = false;
  if ( table.find( name ) == table.end() )
  {
    renderView->topGraphNode()->setParent( topNode ? topNode : mMainViewPort );
    renderView->updateWindowResize( mSize.width(), mSize.height() );
    table[name] = std::move( renderView );
    out = true;
  }

  return out;
}

void QgsFrameGraph::setRenderViewEnabled( const QString &name, bool enable )
{
  setRenderViewEnabled(name, EyeTarget::Left, enable);
  setRenderViewEnabled(name, EyeTarget::Right, enable);
}

void QgsFrameGraph::setRenderViewEnabled( const QString &name, EyeTarget eye, bool enable )
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  auto it = table.find( name );
  if ( it != table.end() )
  {
    (*it).second->setEnabled( enable );
  }
}

QgsAbstractRenderView *QgsFrameGraph::renderView( const QString &name, EyeTarget eye )
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  auto it = table.find( name );
  if ( it != table.end() )
  {
    return (*it).second.get();
  }
  return nullptr;
}

bool QgsFrameGraph::isRenderViewEnabled( const QString &name, EyeTarget eye )
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  auto it = table.find( name );
  return ( it != table.end() && (*it).second->isEnabled() );
}

void QgsFrameGraph::updateAmbientOcclusionSettings( const QgsAmbientOcclusionSettings &settings )
{
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
  {
    if (!isEyeTargetValid(eye))
      continue;

    QgsAmbientOcclusionRenderView &aoRenderView = ambientOcclusionRenderView(eye);

    aoRenderView.setRadius( settings.radius() );
    aoRenderView.setIntensity( settings.intensity() );
    aoRenderView.setThreshold( settings.threshold() );
    aoRenderView.setEnabled( settings.isEnabled() );
  }

  mLeftRenderData.mPostprocessingEntity->setAmbientOcclusionEnabled(settings.isEnabled());
  if (mRightRenderData.isValid())
    mRightRenderData.mPostprocessingEntity->setAmbientOcclusionEnabled(settings.isEnabled());
}

void QgsFrameGraph::updateEyeDomeSettings( const Qgs3DMapSettings &settings )
{
  mLeftRenderData.mPostprocessingEntity->setEyeDomeLightingEnabled( settings.eyeDomeLightingEnabled() );
  mLeftRenderData.mPostprocessingEntity->setEyeDomeLightingStrength( settings.eyeDomeLightingStrength() );
  mLeftRenderData.mPostprocessingEntity->setEyeDomeLightingDistance( settings.eyeDomeLightingDistance() );
  if (mRightRenderData.isValid())
  {
    mRightRenderData.mPostprocessingEntity->setEyeDomeLightingEnabled( settings.eyeDomeLightingEnabled() );
    mRightRenderData.mPostprocessingEntity->setEyeDomeLightingStrength( settings.eyeDomeLightingStrength() );
    mRightRenderData.mPostprocessingEntity->setEyeDomeLightingDistance( settings.eyeDomeLightingDistance() );
  }
}

void QgsFrameGraph::updateShadowSettings( const QgsShadowSettings &shadowSettings, const QList<QgsLightSource *> &lightSources )
{
  if ( shadowSettings.renderShadows() )
  {
    int selectedLight = shadowSettings.selectedDirectionalLight();
    QgsDirectionalLightSettings *light = nullptr;
    for ( int i = 0, dirLight = 0; !light && i < lightSources.size(); i++ )
    {
      if ( lightSources[i]->type() == Qgis::LightSourceType::Directional )
      {
        if ( dirLight == selectedLight )
          light = qgis::down_cast< QgsDirectionalLightSettings * >( lightSources[i] );
        dirLight++;
      }
    }

    if ( light )
    {
      for (auto eye : {EyeTarget::Left, EyeTarget::Right})
      {
        if (isEyeTargetValid(eye))
        {
          shadowRenderView(eye).setMapSize( shadowSettings.shadowMapResolution(), shadowSettings.shadowMapResolution() );
          shadowRenderView(eye).setEnabled( true );
        }
      }
      mLeftRenderData.mPostprocessingEntity->setShadowRenderingEnabled( true );
      mLeftRenderData.mPostprocessingEntity->setShadowBias( static_cast<float>( shadowSettings.shadowBias() ) );
      mLeftRenderData.mPostprocessingEntity->updateShadowSettings( *light, static_cast<float>( shadowSettings.maximumShadowRenderingDistance() ) );
      if (mRightRenderData.isValid())
      {
        mRightRenderData.mPostprocessingEntity->setShadowRenderingEnabled( true );
        mRightRenderData.mPostprocessingEntity->setShadowBias( static_cast<float>( shadowSettings.shadowBias() ) );
        mRightRenderData.mPostprocessingEntity->updateShadowSettings( *light, static_cast<float>( shadowSettings.maximumShadowRenderingDistance() ) );
      }
    }
  }
  else
  {
    for (auto eye : {EyeTarget::Left, EyeTarget::Right})
      if (isEyeTargetValid(eye))
        shadowRenderView(eye).setEnabled( false );
    mLeftRenderData.mPostprocessingEntity->setShadowRenderingEnabled( false );
    if (mRightRenderData.isValid())
      mRightRenderData.mPostprocessingEntity->setShadowRenderingEnabled( false );
  }
}

void QgsFrameGraph::updateDebugShadowMapSettings( const Qgs3DMapSettings &settings )
{
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
  {
    if (!isEyeTargetValid(eye))
      continue;

    auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
    auto &tex = eye == EyeTarget::Left ? mLeftRenderData.mShadowTextureDebugging : mRightRenderData.mShadowTextureDebugging;
    QgsDebugTextureRenderView *debugRenderView = dynamic_cast<QgsDebugTextureRenderView *>( table[DEBUG_RENDERVIEW].get() );
    if ( !tex && settings.debugShadowMapEnabled() )
    {
      Qt3DRender::QTexture2D *shadowDepthTexture = shadowRenderView(eye).mapTexture();
      tex = new QgsDebugTextureEntity( shadowDepthTexture, debugRenderView->debugLayer(), this );
    }

    debugRenderView->setEnabled( settings.debugShadowMapEnabled() || settings.debugDepthMapEnabled() );

    if ( tex )
    {
      tex->setEnabled( settings.debugShadowMapEnabled() );
      if ( settings.debugShadowMapEnabled() )
        tex->setPosition( settings.debugShadowMapCorner(), settings.debugShadowMapSize() );
      else
      {
        delete tex;
        tex = nullptr;
      }
    }
  }
}

void QgsFrameGraph::updateDebugDepthMapSettings( const Qgs3DMapSettings &settings )
{
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
  {
    if (!isEyeTargetValid(eye))
      continue;

    auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
    auto &tex = eye == EyeTarget::Left ? mLeftRenderData.mDepthTextureDebugging : mRightRenderData.mDepthTextureDebugging;
    QgsDebugTextureRenderView *debugRenderView = dynamic_cast<QgsDebugTextureRenderView *>( table[DEBUG_RENDERVIEW].get() );
    if ( !tex && settings.debugDepthMapEnabled() )
    {
      Qt3DRender::QTexture2D *forwardDepthTexture = forwardRenderView(eye).depthTexture();
      tex = new QgsDebugTextureEntity( forwardDepthTexture, debugRenderView->debugLayer(), this );
    }

    debugRenderView->setEnabled( settings.debugShadowMapEnabled() || settings.debugDepthMapEnabled() );

    if ( tex )
    {
      tex->setEnabled( settings.debugDepthMapEnabled() );
      if ( settings.debugDepthMapEnabled() )
        tex->setPosition( settings.debugDepthMapCorner(), settings.debugDepthMapSize() );
      else
      {
        delete tex;
        tex = nullptr;
      }
    }
  }
}

QString QgsFrameGraph::dumpFrameGraph() const
{
  QObject *top = mRenderSurfaceSelector;
  while ( top->parent() && dynamic_cast<Qt3DRender::QFrameGraphNode *>( top->parent() ) )
    top = top->parent();

  QgsFrameGraphUtils::FgDumpContext context;
  context.lowestId = mMainCamera->id().id();
  QStringList strList = QgsFrameGraphUtils::dumpFrameGraph( dynamic_cast<Qt3DRender::QFrameGraphNode *>( top ), context );

  return strList.join( "\n" ) + QString( "\n" );
}

QString QgsFrameGraph::dumpSceneGraph() const
{
  QStringList strList = QgsFrameGraphUtils::dumpSceneGraph( mRootEntity, QgsFrameGraphUtils::FgDumpContext() );
  return strList.join( "\n" ) + QString( "\n" );
}

void QgsFrameGraph::setClearColor( const QColor &clearColor )
{
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
    if (isEyeTargetValid(eye))
      forwardRenderView(eye).setClearColor( clearColor );
}

void QgsFrameGraph::setFrustumCullingEnabled( bool enabled )
{
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
    if (isEyeTargetValid(eye))
      forwardRenderView(eye).setFrustumCullingEnabled( enabled );
}

void QgsFrameGraph::setSize( QSize s )
{
  mSize = s;
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
  {
    if (!isEyeTargetValid(eye))
      continue;

    auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
    for ( auto it = table.begin(); it != table.end(); ++it )
    {
      QgsAbstractRenderView *rv = it->second.get();
      rv->updateWindowResize( mSize.width(), mSize.height() );
    }
    auto &captureData = eye == EyeTarget::Left ? mLeftRenderData : mRightRenderData;
    if (captureData.isValid())
    {
      captureData.mColorCaptureTexture->setSize( mSize.width(), mSize.height() );
      captureData.mDepthCaptureTexture->setSize( mSize.width(), mSize.height() );
    }
  }

  mRenderSurfaceSelector->setExternalRenderTargetSize( mSize );
}

Qt3DRender::QRenderCapture *QgsFrameGraph::renderCapture(EyeTarget eye)
{
  auto &captureData = eye == EyeTarget::Left ? mLeftRenderData : mRightRenderData;
  return captureData.mRenderCapture;
}

void QgsFrameGraph::setRenderCaptureEnabled( bool enabled )
{
  if ( enabled == mRenderCaptureEnabled )
    return;
  mRenderCaptureEnabled = enabled;
  mLeftRenderData.mSelector->setEnabled( mRenderCaptureEnabled );
  if (mRightRenderData.isValid())
    mRightRenderData.mSelector->setEnabled( mRenderCaptureEnabled );
}

void QgsFrameGraph::setDebugOverlayEnabled( bool enabled )
{
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
    if (isEyeTargetValid(eye))
      forwardRenderView(eye).setDebugOverlayEnabled( enabled );
}

void QgsFrameGraph::removeClipPlanes()
{
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
    if (isEyeTargetValid(eye))
      forwardRenderView(eye).removeClipPlanes();
}

void QgsFrameGraph::addClipPlanes( int nrClipPlanes )
{
  for (auto eye : {EyeTarget::Left, EyeTarget::Right})
    if (isEyeTargetValid(eye))
      forwardRenderView(eye).addClipPlanes( nrClipPlanes );
}

QgsForwardRenderView &QgsFrameGraph::forwardRenderView(EyeTarget eye)
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  QgsAbstractRenderView *rv = table[QgsFrameGraph::FORWARD_RENDERVIEW].get();
  return *( dynamic_cast<QgsForwardRenderView *>( rv ) );
}

QgsShadowRenderView &QgsFrameGraph::shadowRenderView(EyeTarget eye)
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  QgsAbstractRenderView *rv = table[QgsFrameGraph::SHADOW_RENDERVIEW].get();
  return *( dynamic_cast<QgsShadowRenderView *>( rv ) );
}

QgsDepthRenderView &QgsFrameGraph::depthRenderView(EyeTarget eye)
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  QgsAbstractRenderView *rv = table[QgsFrameGraph::DEPTH_RENDERVIEW].get();
  return *( dynamic_cast<QgsDepthRenderView *>( rv ) );
}

QgsAmbientOcclusionRenderView &QgsFrameGraph::ambientOcclusionRenderView(EyeTarget eye)
{
  auto &table = eye == EyeTarget::Left ? mLeftEyeRenderViewMap : mRightEyeRenderViewMap;
  QgsAbstractRenderView *rv = table[QgsFrameGraph::AMBIENT_OCCLUSION_RENDERVIEW].get();
  return *( dynamic_cast<QgsAmbientOcclusionRenderView *>( rv ) );
}
