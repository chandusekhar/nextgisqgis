/***************************************************************************
  qgsmaprendererjob.cpp
  --------------------------------------
  Date                 : December 2013
  Copyright            : (C) 2013 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsmaprendererjob.h"

#include <QPainter>
#include <QElapsedTimer>
#include <QTimer>
#include <QtConcurrentMap>

#include "qgslogger.h"
#include "qgsrendercontext.h"
#include "qgsmaplayer.h"
#include "qgsproject.h"
#include "qgsmaplayerrenderer.h"
#include "qgsmaplayerstylemanager.h"
#include "qgsmaprenderercache.h"
#include "qgsmessagelog.h"
#include "qgspallabeling.h"
#include "qgsvectorlayerrenderer.h"
#include "qgsvectorlayer.h"
#include "qgsexception.h"
#include "qgslabelingengine.h"
#include "qgsmaplayerlistutils.h"
#include "qgsvectorlayerlabeling.h"
#include "qgssettings.h"
#include "qgsexpressioncontextutils.h"
#include "qgssymbol.h"
#include "qgsrenderer.h"
#include "qgssymbollayer.h"
#include "qgsvectorlayerutils.h"
#include "qgssymbollayerutils.h"

///@cond PRIVATE

const QString QgsMapRendererJob::LABEL_CACHE_ID = QStringLiteral( "_labels_" );

QgsMapRendererJob::QgsMapRendererJob( const QgsMapSettings &settings )
  : mSettings( settings )

{
}


QgsMapRendererQImageJob::QgsMapRendererQImageJob( const QgsMapSettings &settings )
  : QgsMapRendererJob( settings )
{
}


QgsMapRendererJob::Errors QgsMapRendererJob::errors() const
{
  return mErrors;
}

void QgsMapRendererJob::setCache( QgsMapRendererCache *cache )
{
  mCache = cache;
}

QHash<QgsMapLayer *, int> QgsMapRendererJob::perLayerRenderingTime() const
{
  QHash<QgsMapLayer *, int> result;
  for ( auto it = mPerLayerRenderingTime.constBegin(); it != mPerLayerRenderingTime.constEnd(); ++it )
  {
    if ( it.key() )
      result.insert( it.key(), it.value() );
  }
  return result;
}

const QgsMapSettings &QgsMapRendererJob::mapSettings() const
{
  return mSettings;
}

bool QgsMapRendererJob::prepareLabelCache() const
{
  bool canCache = mCache;

  // calculate which layers will be labeled
  QSet< QgsMapLayer * > labeledLayers;
  const QList<QgsMapLayer *> layers = mSettings.layers();
  for ( const QgsMapLayer *ml : layers )
  {
    QgsVectorLayer *vl = const_cast< QgsVectorLayer * >( qobject_cast<const QgsVectorLayer *>( ml ) );
    if ( vl && QgsPalLabeling::staticWillUseLayer( vl ) )
      labeledLayers << vl;
    if ( vl && vl->labelsEnabled() && vl->labeling()->requiresAdvancedEffects() )
    {
      canCache = false;
      break;
    }
  }

  if ( mCache && mCache->hasCacheImage( LABEL_CACHE_ID ) )
  {
    // we may need to clear label cache and re-register labeled features - check for that here

    // can we reuse the cached label solution?
    bool canUseCache = canCache && mCache->dependentLayers( LABEL_CACHE_ID ).toSet() == labeledLayers;
    if ( !canUseCache )
    {
      // no - participating layers have changed
      mCache->clearCacheImage( LABEL_CACHE_ID );
    }
  }
  return canCache;
}


bool QgsMapRendererJob::reprojectToLayerExtent( const QgsMapLayer *ml, const QgsCoordinateTransform &ct, QgsRectangle &extent, QgsRectangle &r2 )
{
  bool split = false;

  // we can safely use ballpark transforms without bothering the user here -- at the likely scale of layer extents there
  // won't be an appreciable difference, and we aren't actually transforming any rendered points here anyway (just the layer extent)
  QgsCoordinateTransform approxTransform = ct;
  approxTransform.setBallparkTransformsAreAppropriate( true );

  try
  {
#ifdef QGISDEBUG
    // QgsLogger::debug<QgsRectangle>("Getting extent of canvas in layers CS. Canvas is ", extent, __FILE__, __FUNCTION__, __LINE__);
#endif
    // Split the extent into two if the source CRS is
    // geographic and the extent crosses the split in
    // geographic coordinates (usually +/- 180 degrees,
    // and is assumed to be so here), and draw each
    // extent separately.
    static const double SPLIT_COORD = 180.0;

    if ( ml->crs().isGeographic() )
    {
      if ( ml->type() == QgsMapLayerType::VectorLayer && !approxTransform.destinationCrs().isGeographic() )
      {
        // if we transform from a projected coordinate system check
        // check if transforming back roughly returns the input
        // extend - otherwise render the world.
        QgsRectangle extent1 = approxTransform.transformBoundingBox( extent, QgsCoordinateTransform::ReverseTransform );
        QgsRectangle extent2 = approxTransform.transformBoundingBox( extent1, QgsCoordinateTransform::ForwardTransform );

        QgsDebugMsgLevel( QStringLiteral( "\n0:%1 %2x%3\n1:%4\n2:%5 %6x%7 (w:%8 h:%9)" )
                          .arg( extent.toString() ).arg( extent.width() ).arg( extent.height() )
                          .arg( extent1.toString(), extent2.toString() ).arg( extent2.width() ).arg( extent2.height() )
                          .arg( std::fabs( 1.0 - extent2.width() / extent.width() ) )
                          .arg( std::fabs( 1.0 - extent2.height() / extent.height() ) )
                          , 3 );

        // can differ by a maximum of up to 20% of height/width
        if ( qgsDoubleNear( extent2.xMinimum(), extent.xMinimum(), extent.width() * 0.2 )
             && qgsDoubleNear( extent2.xMaximum(), extent.xMaximum(), extent.width() * 0.2 )
             && qgsDoubleNear( extent2.yMinimum(), extent.yMinimum(), extent.height() * 0.2 )
             && qgsDoubleNear( extent2.yMaximum(), extent.yMaximum(), extent.height() * 0.2 )
           )
        {
          extent = extent1;
        }
        else
        {
          extent = QgsRectangle( -180.0, -90.0, 180.0, 90.0 );
        }
      }
      else
      {
        // Note: ll = lower left point
        QgsPointXY ll = approxTransform.transform( extent.xMinimum(), extent.yMinimum(),
                        QgsCoordinateTransform::ReverseTransform );

        //   and ur = upper right point
        QgsPointXY ur = approxTransform.transform( extent.xMaximum(), extent.yMaximum(),
                        QgsCoordinateTransform::ReverseTransform );

        QgsDebugMsgLevel( QStringLiteral( "in:%1 (ll:%2 ur:%3)" ).arg( extent.toString(), ll.toString(), ur.toString() ), 4 );

        extent = approxTransform.transformBoundingBox( extent, QgsCoordinateTransform::ReverseTransform );

        QgsDebugMsgLevel( QStringLiteral( "out:%1 (w:%2 h:%3)" ).arg( extent.toString() ).arg( extent.width() ).arg( extent.height() ), 4 );

        if ( ll.x() > ur.x() )
        {
          // the coordinates projected in reverse order than what one would expect.
          // we are probably looking at an area that includes longitude of 180 degrees.
          // we need to take into account coordinates from two intervals: (-180,x1) and (x2,180)
          // so let's use (-180,180). This hopefully does not add too much overhead. It is
          // more straightforward than rendering with two separate extents and more consistent
          // for rendering, labeling and caching as everything is rendered just in one go
          extent.setXMinimum( -SPLIT_COORD );
          extent.setXMaximum( SPLIT_COORD );
        }
      }

      // TODO: the above rule still does not help if using a projection that covers the whole
      // world. E.g. with EPSG:3857 the longitude spectrum -180 to +180 is mapped to approx.
      // -2e7 to +2e7. Converting extent from -5e7 to +5e7 is transformed as -90 to +90,
      // but in fact the extent should cover the whole world.
    }
    else // can't cross 180
    {
      if ( approxTransform.destinationCrs().isGeographic() &&
           ( extent.xMinimum() <= -180 || extent.xMaximum() >= 180 ||
             extent.yMinimum() <= -90 || extent.yMaximum() >= 90 ) )
        // Use unlimited rectangle because otherwise we may end up transforming wrong coordinates.
        // E.g. longitude -200 to +160 would be understood as +40 to +160 due to periodicity.
        // We could try to clamp coords to (-180,180) for lon resp. (-90,90) for lat,
        // but this seems like a safer choice.
        extent = QgsRectangle( std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() );
      else
        extent = approxTransform.transformBoundingBox( extent, QgsCoordinateTransform::ReverseTransform );
    }
  }
  catch ( QgsCsException &cse )
  {
    Q_UNUSED( cse )
    QgsDebugMsg( QStringLiteral( "Transform error caught" ) );
    extent = QgsRectangle( std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() );
    r2     = QgsRectangle( std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() );
  }

  return split;
}

QImage *QgsMapRendererJob::allocateImage( QString layerId )
{
  QImage *image = new QImage( mSettings.deviceOutputSize(),
                              mSettings.outputImageFormat() );
  image->setDevicePixelRatio( static_cast<qreal>( mSettings.devicePixelRatio() ) );
  if ( image->isNull() )
  {
    mErrors.append( Error( layerId, tr( "Insufficient memory for image %1x%2" ).arg( mSettings.outputSize().width() ).arg( mSettings.outputSize().height() ) ) );
    delete image;
    return nullptr;
  }
  return image;
}

QPainter *QgsMapRendererJob::allocateImageAndPainter( QString layerId, QImage *&image )
{
  QPainter *painter = nullptr;
  image = allocateImage( layerId );
  if ( image )
  {
    painter = new QPainter( image );
    painter->setRenderHint( QPainter::Antialiasing, mSettings.testFlag( QgsMapSettings::Antialiasing ) );
  }
  return painter;
}

LayerRenderJobs QgsMapRendererJob::prepareJobs( QPainter *painter, QgsLabelingEngine *labelingEngine2, bool deferredPainterSet )
{
  LayerRenderJobs layerJobs;

  // render all layers in the stack, starting at the base
  QListIterator<QgsMapLayer *> li( mSettings.layers() );
  li.toBack();

  if ( mCache )
  {
    bool cacheValid = mCache->init( mSettings.visibleExtent(), mSettings.scale() );
    Q_UNUSED( cacheValid )
    QgsDebugMsgLevel( QStringLiteral( "CACHE VALID: %1" ).arg( cacheValid ), 4 );
  }

  bool requiresLabelRedraw = !( mCache && mCache->hasCacheImage( LABEL_CACHE_ID ) );

  while ( li.hasPrevious() )
  {
    QgsMapLayer *ml = li.previous();

    QgsDebugMsgLevel( QStringLiteral( "layer %1:  minscale:%2  maxscale:%3  scaledepvis:%4  blendmode:%5 isValid:%6" )
                      .arg( ml->name() )
                      .arg( ml->minimumScale() )
                      .arg( ml->maximumScale() )
                      .arg( ml->hasScaleBasedVisibility() )
                      .arg( ml->blendMode() )
                      .arg( ml->isValid() )
                      , 3 );

    if ( !ml->isValid() )
    {
      QgsDebugMsgLevel( QStringLiteral( "Invalid Layer skipped" ), 3 );
      continue;
    }

    if ( !ml->isInScaleRange( mSettings.scale() ) ) //|| mOverview )
    {
      QgsDebugMsgLevel( QStringLiteral( "Layer not rendered because it is not within the defined visibility scale range" ), 3 );
      continue;
    }

    QgsRectangle r1 = mSettings.visibleExtent(), r2;
    r1.grow( mSettings.extentBuffer() );
    QgsCoordinateTransform ct;

    ct = mSettings.layerTransform( ml );
    if ( ct.isValid() )
    {
      reprojectToLayerExtent( ml, ct, r1, r2 );
    }
    QgsDebugMsgLevel( "extent: " + r1.toString(), 3 );
    if ( !r1.isFinite() || !r2.isFinite() )
    {
      mErrors.append( Error( ml->id(), tr( "There was a problem transforming the layer's extent. Layer skipped." ) ) );
      continue;
    }

    QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( ml );

    // Force render of layers that are being edited
    // or if there's a labeling engine that needs the layer to register features
    if ( mCache && vl )
    {
      bool requiresLabeling = false;
      requiresLabeling = ( labelingEngine2 && QgsPalLabeling::staticWillUseLayer( vl ) ) && requiresLabelRedraw;
      if ( vl->isEditable() || requiresLabeling )
      {
        mCache->clearCacheImage( ml->id() );
      }
    }

    layerJobs.append( LayerRenderJob() );
    LayerRenderJob &job = layerJobs.last();
    job.cached = false;
    job.img = nullptr;
    job.layer = ml;
    job.layerId = ml->id();
    job.renderingTime = -1;

    job.context = QgsRenderContext::fromMapSettings( mSettings );
    job.context.expressionContext().appendScope( QgsExpressionContextUtils::layerScope( ml ) );
    job.context.setPainter( painter );
    job.context.setLabelingEngine( labelingEngine2 );
    job.context.setCoordinateTransform( ct );
    job.context.setExtent( r1 );

    if ( mFeatureFilterProvider )
      job.context.setFeatureFilterProvider( mFeatureFilterProvider );

    QgsMapLayerStyleOverride styleOverride( ml );
    if ( mSettings.layerStyleOverrides().contains( ml->id() ) )
      styleOverride.setOverrideStyle( mSettings.layerStyleOverrides().value( ml->id() ) );

    job.blendMode = ml->blendMode();
    job.opacity = 1.0;
    if ( vl )
    {
      job.opacity = vl->opacity();
    }

    // if we can use the cache, let's do it and avoid rendering!
    if ( mCache && mCache->hasCacheImage( ml->id() ) )
    {
      job.cached = true;
      job.imageInitialized = true;
      job.img = new QImage( mCache->cacheImage( ml->id() ) );
      job.img->setDevicePixelRatio( static_cast<qreal>( mSettings.devicePixelRatio() ) );
      job.renderer = nullptr;
      job.context.setPainter( nullptr );
      continue;
    }

    // If we are drawing with an alternative blending mode then we need to render to a separate image
    // before compositing this on the map. This effectively flattens the layer and prevents
    // blending occurring between objects on the layer
    if ( mCache || ( !painter && !deferredPainterSet ) || needTemporaryImage( ml ) )
    {
      // Flattened image for drawing when a blending mode is set
      job.context.setPainter( allocateImageAndPainter( ml->id(), job.img ) );
      if ( ! job.img )
      {
        layerJobs.removeLast();
        continue;
      }
    }

    QElapsedTimer layerTime;
    layerTime.start();
    job.renderer = ml->createMapRenderer( job.context );
    job.renderingTime = layerTime.elapsed(); // include job preparation time in layer rendering time
  } // while (li.hasPrevious())

  return layerJobs;
}

LayerRenderJobs QgsMapRendererJob::prepareSecondPassJobs( LayerRenderJobs &firstPassJobs, LabelRenderJob &labelJob )
{
  LayerRenderJobs secondPassJobs;

  // We will need to quickly access the associated rendering job of a layer
  QHash<QString, LayerRenderJob *> layerJobMapping;

  // ... and whether a layer has a mask defined
  QSet<QString> layerHasMask;

  struct MaskSource
  {
    QString layerId;
    QString labelRuleId;
    int labelMaskId;
    MaskSource( const QString &layerId_, const QString &labelRuleId_, int labelMaskId_ ):
      layerId( layerId_ ), labelRuleId( labelRuleId_ ), labelMaskId( labelMaskId_ ) {}
  };

  // We collect for each layer, the set of symbol layers that will be "masked"
  // and the list of source layers that have a mask
  QHash<QString, QPair<QSet<QgsSymbolLayerId>, QList<MaskSource>>> maskedSymbolLayers;

  for ( LayerRenderJob &job : firstPassJobs )
  {
    QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( job.layer );
    if ( ! vl )
      continue;

    layerJobMapping[job.layer->id()] = &job;

    // lambda function to factor code for both label masks and symbol layer masks
    auto collectMasks = [&]( QHash<QString, QSet<QgsSymbolLayerId>> *masks, QString sourceLayerId, QString ruleId = QString(), int labelMaskId = -1 )
    {
      for ( auto it = masks->begin(); it != masks->end(); ++it )
      {
        auto lit = maskedSymbolLayers.find( it.key() );
        if ( lit == maskedSymbolLayers.end() )
        {
          maskedSymbolLayers[it.key()] = qMakePair( it.value(), QList<MaskSource>() << MaskSource( sourceLayerId, ruleId, labelMaskId ) );
        }
        else
        {
          if ( lit->first != it.value() )
          {
            QgsLogger::warning( QStringLiteral( "Layer %1 : Different sets of symbol layers are masked by different sources ! Only one (arbitrary) set will be retained !" ).arg( it.key() ) );
            continue;
          }
          lit->second.push_back( MaskSource( sourceLayerId, ruleId, labelMaskId ) );
        }
      }
      if ( ! masks->isEmpty() )
        layerHasMask.insert( sourceLayerId );
    };

    // collect label masks
    QHash<QString, QHash<QString, QSet<QgsSymbolLayerId>>> labelMasks = QgsVectorLayerUtils::labelMasks( vl );
    for ( auto it = labelMasks.begin(); it != labelMasks.end(); it++ )
    {
      QString labelRule = it.key();
      QHash<QString, QSet<QgsSymbolLayerId>> masks = it.value();

      // group layers by QSet<QgsSymbolLayerReference>
      QSet<QgsSymbolLayerReference> slRefs;
      for ( auto mit = masks.begin(); mit != masks.end(); mit++ )
      {
        for ( auto slIt = mit.value().begin(); slIt != mit.value().end(); slIt++ )
        {
          slRefs.insert( QgsSymbolLayerReference( mit.key(), *slIt ) );
        }
      }
      // generate a new mask id for this set
      int labelMaskId = labelJob.maskIdProvider.insertLabelLayer( vl->id(), it.key(), slRefs );

      // now collect masks
      collectMasks( &masks, vl->id(), labelRule, labelMaskId );
    }

    // collect symbol layer masks
    QHash<QString, QSet<QgsSymbolLayerId>> symbolLayerMasks = QgsVectorLayerUtils::symbolLayerMasks( vl );
    collectMasks( &symbolLayerMasks, vl->id() );
  }

  if ( maskedSymbolLayers.isEmpty() )
    return secondPassJobs;

  // Now that we know some layers have a mask, we have to allocate a mask image and painter
  // for them in the first pass job
  for ( LayerRenderJob &job : firstPassJobs )
  {
    QgsMapLayer *ml = job.layer;

    if ( job.img == nullptr )
    {
      job.context.setPainter( allocateImageAndPainter( ml->id(), job.img ) );
    }
    if ( layerHasMask.contains( ml->id() ) )
    {
      // Note: we only need an alpha channel here, rather than a full RGBA image
      job.context.setMaskPainter( allocateImageAndPainter( ml->id(), job.maskImage ) );
      job.maskImage->fill( 0 );
    }
  }

  // Allocate an image for labels
  if ( labelJob.img == nullptr )
  {
    labelJob.img = allocateImage( QStringLiteral( "labels" ) );
  }

  // Prepare label mask images
  for ( int maskId = 0; maskId < labelJob.maskIdProvider.size(); maskId++ )
  {
    QImage *maskImage;
    labelJob.context.setMaskPainter( allocateImageAndPainter( QStringLiteral( "label mask" ), maskImage ), maskId );
    maskImage->fill( 0 );
    labelJob.maskImages.push_back( maskImage );
  }
  labelJob.context.setMaskIdProvider( &labelJob.maskIdProvider );

  // Prepare second pass jobs
  for ( LayerRenderJob &job : firstPassJobs )
  {
    QgsMapLayer *ml = job.layer;

    auto it = maskedSymbolLayers.find( ml->id() );
    if ( it == maskedSymbolLayers.end() )
      continue;

    QList<MaskSource> &sourceList = it->second;
    const QSet<QgsSymbolLayerId> &symbolList = it->first;

    // copy the initial job ...
    secondPassJobs.append( LayerRenderJob() );
    LayerRenderJob &job2 = secondPassJobs.last();
    job2 = job;
    job2.cached = false;
    job2.firstPassJob = &job;
    QgsVectorLayer *vl1 = qobject_cast<QgsVectorLayer *>( job.layer );

    // ... but clear the image
    job2.context.setMaskPainter( nullptr );
    job2.context.setPainter( allocateImageAndPainter( vl1->id(), job2.img ) );
    if ( ! job2.img )
    {
      secondPassJobs.removeLast();
      continue;
    }

    // Points to the first pass job. This will be needed during the second pass composition.
    for ( MaskSource &source : sourceList )
    {
      if ( source.labelMaskId != -1 )
        job2.maskJobs.push_back( qMakePair( nullptr, source.labelMaskId ) );
      else
        job2.maskJobs.push_back( qMakePair( layerJobMapping[source.layerId], -1 ) );
    }

    // FIXME: another possibility here, to avoid allocating a new map renderer and reuse the one from
    // the first pass job, would be to be able to call QgsMapLayerRenderer::render() with a QgsRenderContext.
    QgsVectorLayerRenderer *mapRenderer = static_cast<QgsVectorLayerRenderer *>( vl1->createMapRenderer( job2.context ) );
    job2.renderer = mapRenderer;

    // Modify the render context so that symbol layers get disabled as needed.
    // The map renderer stores a reference to the context, so we can modify it even after the map renderer creation (what we need here)
    job2.context.setDisabledSymbolLayers( QgsSymbolLayerUtils::toSymbolLayerPointers( mapRenderer->featureRenderer(), symbolList ) );
  }

  return secondPassJobs;
}

LabelRenderJob QgsMapRendererJob::prepareLabelingJob( QPainter *painter, QgsLabelingEngine *labelingEngine2, bool canUseLabelCache )
{
  LabelRenderJob job;
  job.context = QgsRenderContext::fromMapSettings( mSettings );
  job.context.setPainter( painter );
  job.context.setLabelingEngine( labelingEngine2 );
  job.context.setExtent( mSettings.visibleExtent() );
  job.context.setFeatureFilterProvider( mFeatureFilterProvider );

  // if we can use the cache, let's do it and avoid rendering!
  bool hasCache = canUseLabelCache && mCache && mCache->hasCacheImage( LABEL_CACHE_ID );
  if ( hasCache )
  {
    job.cached = true;
    job.complete = true;
    job.img = new QImage( mCache->cacheImage( LABEL_CACHE_ID ) );
    Q_ASSERT( job.img->devicePixelRatio() == mSettings.devicePixelRatio() );
    job.context.setPainter( nullptr );
  }
  else
  {
    if ( canUseLabelCache && ( mCache || !painter ) )
    {
      job.img = allocateImage( QStringLiteral( "labels" ) );
    }
  }

  return job;
}


void QgsMapRendererJob::cleanupJobs( LayerRenderJobs &jobs )
{
  for ( LayerRenderJobs::iterator it = jobs.begin(); it != jobs.end(); ++it )
  {
    LayerRenderJob &job = *it;
    if ( job.img )
    {
      delete job.context.painter();
      job.context.setPainter( nullptr );

      if ( mCache && !job.cached && !job.context.renderingStopped() && job.layer )
      {
        QgsDebugMsgLevel( QStringLiteral( "caching image for %1" ).arg( job.layerId ), 2 );
        mCache->setCacheImage( job.layerId, *job.img, QList< QgsMapLayer * >() << job.layer );
      }

      delete job.img;
      job.img = nullptr;
    }

    // delete the mask image and painter
    if ( job.maskImage )
    {
      delete job.context.maskPainter();
      job.context.setMaskPainter( nullptr );
      delete job.maskImage;
    }

    if ( job.renderer )
    {
      const auto constErrors = job.renderer->errors();
      for ( const QString &message : constErrors )
        mErrors.append( Error( job.renderer->layerId(), message ) );

      delete job.renderer;
      job.renderer = nullptr;
    }

    if ( job.layer )
      mPerLayerRenderingTime.insert( job.layer, job.renderingTime );
  }

  jobs.clear();
}

void QgsMapRendererJob::cleanupSecondPassJobs( LayerRenderJobs &jobs )
{
  for ( auto &job : jobs )
  {
    if ( job.img )
    {
      delete job.context.painter();
      job.context.setPainter( nullptr );

      delete job.img;
      job.img = nullptr;
    }

    if ( job.renderer )
    {
      delete job.renderer;
      job.renderer = nullptr;
    }

    if ( job.layer )
      mPerLayerRenderingTime.insert( job.layer, job.renderingTime );
  }

  jobs.clear();
}

void QgsMapRendererJob::cleanupLabelJob( LabelRenderJob &job )
{
  if ( job.img )
  {
    if ( mCache && !job.cached && !job.context.renderingStopped() )
    {
      QgsDebugMsgLevel( QStringLiteral( "caching label result image" ), 2 );
      mCache->setCacheImage( LABEL_CACHE_ID, *job.img, _qgis_listQPointerToRaw( job.participatingLayers ) );
    }

    delete job.img;
    job.img = nullptr;
  }

  for ( int maskId = 0; maskId < job.maskImages.size(); maskId++ )
  {
    delete job.context.maskPainter( maskId );
    job.context.setMaskPainter( nullptr, maskId );
    delete job.maskImages[maskId];
  }
}


#define DEBUG_RENDERING 0

QImage QgsMapRendererJob::composeImage( const QgsMapSettings &settings, const LayerRenderJobs &jobs, const LabelRenderJob &labelJob )
{
  QImage image( settings.deviceOutputSize(), settings.outputImageFormat() );
  image.setDevicePixelRatio( settings.devicePixelRatio() );
  image.fill( settings.backgroundColor().rgba() );

  QPainter painter( &image );

#if DEBUG_RENDERING
  int i = 0;
#endif
  for ( LayerRenderJobs::const_iterator it = jobs.constBegin(); it != jobs.constEnd(); ++it )
  {
    const LayerRenderJob &job = *it;

    if ( job.layer && job.layer->customProperty( QStringLiteral( "rendering/renderAboveLabels" ) ).toBool() )
      continue; // skip layer for now, it will be rendered after labels

    if ( !job.imageInitialized )
      continue; // img not safe to compose

    painter.setCompositionMode( job.blendMode );
    painter.setOpacity( job.opacity );

#if DEBUG_RENDERING
    job.img->save( QString( "/tmp/final_%1.png" ).arg( i ) );
    i++;
#endif
    Q_ASSERT( job.img );

    painter.drawImage( 0, 0, *job.img );
  }

  // IMPORTANT - don't draw labelJob img before the label job is complete,
  // as the image is uninitialized and full of garbage before the label job
  // commences
  if ( labelJob.img && labelJob.complete )
  {
    painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
    painter.setOpacity( 1.0 );
    painter.drawImage( 0, 0, *labelJob.img );
  }

  // render any layers with the renderAboveLabels flag now
  for ( LayerRenderJobs::const_iterator it = jobs.constBegin(); it != jobs.constEnd(); ++it )
  {
    const LayerRenderJob &job = *it;

    if ( !job.layer || !job.layer->customProperty( QStringLiteral( "rendering/renderAboveLabels" ) ).toBool() )
      continue;

    if ( !job.imageInitialized )
      continue; // img not safe to compose

    painter.setCompositionMode( job.blendMode );
    painter.setOpacity( job.opacity );

    Q_ASSERT( job.img );

    painter.drawImage( 0, 0, *job.img );
  }

  painter.end();
#if DEBUG_RENDERING
  image.save( "/tmp/final.png" );
#endif
  return image;
}

void QgsMapRendererJob::composeSecondPass( LayerRenderJobs &secondPassJobs, LabelRenderJob &labelJob )
{
#if DEBUG_RENDERING
  int i = 0;
#endif
  // compose the second pass with the mask
  for ( LayerRenderJob &job : secondPassJobs )
  {
#if DEBUG_RENDERING
    i++;
    job.img->save( QString( "/tmp/second_%1.png" ).arg( i ) );
    int mask = 0;
#endif

    // Merge all mask images into the first one if we have more than one mask image
    if ( job.maskJobs.size() > 1 )
    {
      QPainter *maskPainter = nullptr;
      for ( QPair<LayerRenderJob *, int> p : job.maskJobs )
      {
        QImage *maskImage = p.first ? p.first->maskImage : labelJob.maskImages[p.second];
#if DEBUG_RENDERING
        maskImage->save( QString( "/tmp/mask_%1_%2.png" ).arg( i ).arg( mask++ ) );
#endif
        if ( ! maskPainter )
        {
          maskPainter = p.first ? p.first->context.maskPainter() : labelJob.context.maskPainter( p.second );
        }
        else
        {
          maskPainter->drawImage( 0, 0, *maskImage );
        }
      }
    }

    if ( ! job.maskJobs.isEmpty() )
    {
      // All have been merged into the first
      QPair<LayerRenderJob *, int> p = *job.maskJobs.begin();
      QImage *maskImage = p.first ? p.first->maskImage : labelJob.maskImages[p.second];
#if DEBUG_RENDERING
      maskImage->save( QString( "/tmp/mask_%1.png" ).arg( i ) );
#endif

      // Only retain parts of the second rendering that are "inside" the mask image
      QPainter *painter = job.context.painter();
      painter->setCompositionMode( QPainter::CompositionMode_DestinationIn );
      painter->drawImage( 0, 0, *maskImage );
#if DEBUG_RENDERING
      job.img->save( QString( "/tmp/second_%1_a.png" ).arg( i ) );
#endif

      // Modify the first pass' image ...
      {
        QPainter tempPainter;

        // resue the first pass painter, if available
        QPainter *painter1 = job.firstPassJob->context.painter();
        if ( ! painter1 )
        {
          tempPainter.begin( job.firstPassJob->img );
          painter1 = &tempPainter;
        }
#if DEBUG_RENDERING
        job.firstPassJob->img->save( QString( "/tmp/second_%1_first_pass_1.png" ).arg( i ) );
#endif
        // ... first retain parts that are "outside" the mask image
        painter1->setCompositionMode( QPainter::CompositionMode_DestinationOut );
        painter1->drawImage( 0, 0, *maskImage );

#if DEBUG_RENDERING
        job.firstPassJob->img->save( QString( "/tmp/second_%1_first_pass_2.png" ).arg( i ) );
#endif
        // ... and overpaint the second pass' image on it
        painter1->setCompositionMode( QPainter::CompositionMode_SourceOver );
        painter1->drawImage( 0, 0, *job.img );
#if DEBUG_RENDERING
        job.img->save( QString( "/tmp/second_%1_b.png" ).arg( i ) );
        if ( job.firstPassJob )
          job.firstPassJob->img->save( QString( "/tmp/second_%1_first_pass_3.png" ).arg( i ) );
#endif
      }
    }
  }
}

void QgsMapRendererJob::logRenderingTime( const LayerRenderJobs &jobs, const LayerRenderJobs &secondPassJobs, const LabelRenderJob &labelJob )
{
  QgsSettings settings;
  if ( !settings.value( QStringLiteral( "Map/logCanvasRefreshEvent" ), false ).toBool() )
    return;

  QMultiMap<int, QString> elapsed;
  const auto constJobs = jobs;
  for ( const LayerRenderJob &job : constJobs )
    elapsed.insert( job.renderingTime, job.layerId );
  const auto constSecondPassJobs = secondPassJobs;
  for ( const LayerRenderJob &job : constSecondPassJobs )
    elapsed.insert( job.renderingTime, job.layerId + QString( " (second pass)" ) );

  elapsed.insert( labelJob.renderingTime, tr( "Labeling" ) );

  QList<int> tt( elapsed.uniqueKeys() );
  std::sort( tt.begin(), tt.end(), std::greater<int>() );
  const auto constTt = tt;
  for ( int t : constTt )
  {
    QgsMessageLog::logMessage( tr( "%1 ms: %2" ).arg( t ).arg( QStringList( elapsed.values( t ) ).join( QStringLiteral( ", " ) ) ), tr( "Rendering" ) );
  }
  QgsMessageLog::logMessage( QStringLiteral( "---" ), tr( "Rendering" ) );
}

bool QgsMapRendererJob::needTemporaryImage( QgsMapLayer *ml )
{
  switch ( ml->type() )
  {
    case QgsMapLayerType::VectorLayer:
    {
      QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( ml );
      if ( vl->renderer() && vl->renderer()->forceRasterRender() )
      {
        //raster rendering is forced for this layer
        return true;
      }
      if ( mSettings.testFlag( QgsMapSettings::UseAdvancedEffects ) &&
           ( ( vl->blendMode() != QPainter::CompositionMode_SourceOver )
             || ( vl->featureBlendMode() != QPainter::CompositionMode_SourceOver )
             || ( !qgsDoubleNear( vl->opacity(), 1.0 ) ) ) )
      {
        //layer properties require rasterization
        return true;
      }
      break;
    }
    case QgsMapLayerType::RasterLayer:
    {
      // preview of intermediate raster rendering results requires a temporary output image
      if ( mSettings.testFlag( QgsMapSettings::RenderPartialOutput ) )
        return true;
      break;
    }

    case QgsMapLayerType::MeshLayer:
    case QgsMapLayerType::PluginLayer:
      break;
  }

  return false;
}

void QgsMapRendererJob::drawLabeling( QgsRenderContext &renderContext, QgsLabelingEngine *labelingEngine2, QPainter *painter )
{
  QgsDebugMsgLevel( QStringLiteral( "Draw labeling start" ), 5 );

  QElapsedTimer t;
  t.start();

  // Reset the composition mode before rendering the labels
  painter->setCompositionMode( QPainter::CompositionMode_SourceOver );

  renderContext.setPainter( painter );

  if ( labelingEngine2 )
  {
    labelingEngine2->run( renderContext );
  }

  QgsDebugMsgLevel( QStringLiteral( "Draw labeling took (seconds): %1" ).arg( t.elapsed() / 1000. ), 2 );
}

void QgsMapRendererJob::drawLabeling( const QgsMapSettings &settings, QgsRenderContext &renderContext, QgsLabelingEngine *labelingEngine2, QPainter *painter )
{
  Q_UNUSED( settings )

  drawLabeling( renderContext, labelingEngine2, painter );
}

///@endcond PRIVATE
