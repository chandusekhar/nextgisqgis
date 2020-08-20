/***************************************************************************
  qgs3dutils.cpp
  --------------------------------------
  Date                 : July 2017
  Copyright            : (C) 2017 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgs3dutils.h"

#include "qgslinestring.h"
#include "qgspolygon.h"
#include "qgsfeaturerequest.h"
#include "qgsfeatureiterator.h"
#include "qgsfeature.h"
#include "qgsabstractgeometry.h"
#include "qgsvectorlayer.h"
#include "qgsexpressioncontextutils.h"
#include "qgsfeedback.h"
#include "qgsexpression.h"
#include "qgsexpressionutils.h"
#include "qgsoffscreen3dengine.h"

#include "qgs3dmapscene.h"
#include "qgsabstract3dengine.h"
#include "qgsterraingenerator.h"
#include "qgscameracontroller.h"

#include "qgsline3dsymbol.h"
#include "qgspoint3dsymbol.h"
#include "qgspolygon3dsymbol.h"

#include <Qt3DExtras/QPhongMaterial>

QImage Qgs3DUtils::captureSceneImage( QgsAbstract3DEngine &engine, Qgs3DMapScene *scene )
{
  QImage resImage;
  QEventLoop evLoop;

  auto requestImageFcn = [&engine, scene]
  {
    if ( scene->sceneState() == Qgs3DMapScene::Ready )
    {
      engine.requestCaptureImage();
    }
  };

  auto saveImageFcn = [&evLoop, &resImage]( const QImage & img )
  {
    resImage = img;
    evLoop.quit();
  };

  QMetaObject::Connection conn1 = QObject::connect( &engine, &QgsAbstract3DEngine::imageCaptured, saveImageFcn );
  QMetaObject::Connection conn2;

  if ( scene->sceneState() == Qgs3DMapScene::Ready )
  {
    requestImageFcn();
  }
  else
  {
    // first wait until scene is loaded
    conn2 = QObject::connect( scene, &Qgs3DMapScene::sceneStateChanged, requestImageFcn );
  }

  evLoop.exec();

  QObject::disconnect( conn1 );
  if ( conn2 )
    QObject::disconnect( conn2 );

  return resImage;
}

bool Qgs3DUtils::exportAnimation( const Qgs3DAnimationSettings &animationSettings,
                                  const Qgs3DMapSettings &mapSettings,
                                  int framesPerSecond,
                                  const QString &outputDirectory,
                                  const QString &fileNameTemplate,
                                  const QSize &outputSize,
                                  QString &error,
                                  QgsFeedback *feedback
                                )
{
  QgsOffscreen3DEngine engine;
  engine.setSize( outputSize );
  Qgs3DMapScene *scene = new Qgs3DMapScene( mapSettings, &engine );
  engine.setRootEntity( scene );

  if ( animationSettings.keyFrames().size() < 2 )
  {
    error = QObject::tr( "Unable to export 3D animation. Add at least 2 keyframes" );
    return false;
  }

  const float duration = animationSettings.duration(); //in seconds
  if ( duration <= 0 )
  {
    error = QObject::tr( "Unable to export 3D animation (invalid duration)." );
    return false;
  }

  float time = 0;
  int frameNo = 0;
  int totalFrames = static_cast<int>( duration * framesPerSecond );

  if ( fileNameTemplate.isEmpty() )
  {
    error = QObject::tr( "Filename template is empty" );
    return false;
  }

  int numberOfDigits = fileNameTemplate.count( QLatin1Char( '#' ) );
  if ( numberOfDigits < 0 )
  {
    error = QObject::tr( "Wrong filename template format (must contain #)" );
    return false;
  }
  const QString token( numberOfDigits, QLatin1Char( '#' ) );
  if ( !fileNameTemplate.contains( token ) )
  {
    error = QObject::tr( "Filename template must contain all # placeholders in one continuous group." );
    return false;
  }

  while ( time <= duration )
  {

    if ( feedback )
    {
      if ( feedback->isCanceled() )
      {
        error = QObject::tr( "Export canceled" );
        return false;
      }
      feedback->setProgress( frameNo / static_cast<double>( totalFrames ) * 100 );
    }
    ++frameNo;

    Qgs3DAnimationSettings::Keyframe kf = animationSettings.interpolate( time );
    scene->cameraController()->setLookingAtPoint( kf.point, kf.dist, kf.pitch, kf.yaw );

    QString fileName( fileNameTemplate );
    const QString frameNoPaddedLeft( QStringLiteral( "%1" ).arg( frameNo, numberOfDigits, 10, QChar( '0' ) ) ); // e.g. 0001
    fileName.replace( token, frameNoPaddedLeft );
    const QString path = QDir( outputDirectory ).filePath( fileName );

    // It would initially return empty rendered image.
    // Capturing the initial image and throwing it away fixes that.
    // Hopefully we will find a better fix in the future.
    Qgs3DUtils::captureSceneImage( engine, scene );
    QImage img = Qgs3DUtils::captureSceneImage( engine, scene );

    img.save( path );

    time += 1.0f / static_cast<float>( framesPerSecond );
  }

  return true;
}


int Qgs3DUtils::maxZoomLevel( double tile0width, double tileResolution, double maxError )
{
  if ( maxError <= 0 || tileResolution <= 0 || tile0width <= 0 )
    return 0;  // invalid input

  // derived from:
  // tile width [map units] = tile0width / 2^zoomlevel
  // tile error [map units] = tile width / tile resolution
  // + re-arranging to get zoom level if we know tile error we want to get
  double zoomLevel = -log( tileResolution * maxError / tile0width ) / log( 2 );
  return round( zoomLevel );  // we could use ceil() here if we wanted to always get to the desired error
}

QString Qgs3DUtils::altClampingToString( Qgs3DTypes::AltitudeClamping altClamp )
{
  switch ( altClamp )
  {
    case Qgs3DTypes::AltClampAbsolute: return QStringLiteral( "absolute" );
    case Qgs3DTypes::AltClampRelative: return QStringLiteral( "relative" );
    case Qgs3DTypes::AltClampTerrain: return QStringLiteral( "terrain" );
    default: Q_ASSERT( false ); return QString();
  }
}


Qgs3DTypes::AltitudeClamping Qgs3DUtils::altClampingFromString( const QString &str )
{
  if ( str == QLatin1String( "absolute" ) )
    return Qgs3DTypes::AltClampAbsolute;
  else if ( str == QLatin1String( "terrain" ) )
    return Qgs3DTypes::AltClampTerrain;
  else   // "relative"  (default)
    return Qgs3DTypes::AltClampRelative;
}


QString Qgs3DUtils::altBindingToString( Qgs3DTypes::AltitudeBinding altBind )
{
  switch ( altBind )
  {
    case Qgs3DTypes::AltBindVertex: return QStringLiteral( "vertex" );
    case Qgs3DTypes::AltBindCentroid: return QStringLiteral( "centroid" );
    default: Q_ASSERT( false ); return QString();
  }
}


Qgs3DTypes::AltitudeBinding Qgs3DUtils::altBindingFromString( const QString &str )
{
  if ( str == QLatin1String( "vertex" ) )
    return Qgs3DTypes::AltBindVertex;
  else  // "centroid"  (default)
    return Qgs3DTypes::AltBindCentroid;
}

QString Qgs3DUtils::cullingModeToString( Qgs3DTypes::CullingMode mode )
{
  switch ( mode )
  {
    case Qgs3DTypes::NoCulling: return QStringLiteral( "no-culling" );
    case Qgs3DTypes::Front: return QStringLiteral( "front" );
    case Qgs3DTypes::Back: return QStringLiteral( "back" );
    case Qgs3DTypes::FrontAndBack: return QStringLiteral( "front-and-back" );
  }
  return QString();
}

Qgs3DTypes::CullingMode Qgs3DUtils::cullingModeFromString( const QString &str )
{
  if ( str == QStringLiteral( "front" ) )
    return Qgs3DTypes::Front;
  else if ( str == QStringLiteral( "back" ) )
    return Qgs3DTypes::Back;
  else if ( str == QStringLiteral( "front-and-back" ) )
    return Qgs3DTypes::FrontAndBack;
  else
    return Qgs3DTypes::NoCulling;
}

float Qgs3DUtils::clampAltitude( const QgsPoint &p, Qgs3DTypes::AltitudeClamping altClamp, Qgs3DTypes::AltitudeBinding altBind, float height, const QgsPoint &centroid, const Qgs3DMapSettings &map )
{
  float terrainZ = 0;
  if ( altClamp == Qgs3DTypes::AltClampRelative || altClamp == Qgs3DTypes::AltClampTerrain )
  {
    QgsPointXY pt = altBind == Qgs3DTypes::AltBindVertex ? p : centroid;
    terrainZ = map.terrainGenerator()->heightAt( pt.x(), pt.y(), map );
  }

  float geomZ = 0;
  if ( p.is3D() && ( altClamp == Qgs3DTypes::AltClampAbsolute || altClamp == Qgs3DTypes::AltClampRelative ) )
    geomZ = p.z();

  float z = ( terrainZ + geomZ ) * map.terrainVerticalScale() + height;
  return z;
}

void Qgs3DUtils::clampAltitudes( QgsLineString *lineString, Qgs3DTypes::AltitudeClamping altClamp, Qgs3DTypes::AltitudeBinding altBind, const QgsPoint &centroid, float height, const Qgs3DMapSettings &map )
{
  for ( int i = 0; i < lineString->nCoordinates(); ++i )
  {
    float terrainZ = 0;
    if ( altClamp == Qgs3DTypes::AltClampRelative || altClamp == Qgs3DTypes::AltClampTerrain )
    {
      QgsPointXY pt;
      if ( altBind == Qgs3DTypes::AltBindVertex )
      {
        pt.setX( lineString->xAt( i ) );
        pt.setY( lineString->yAt( i ) );
      }
      else
      {
        pt.set( centroid.x(), centroid.y() );
      }
      terrainZ = map.terrainGenerator()->heightAt( pt.x(), pt.y(), map );
    }

    float geomZ = 0;
    if ( altClamp == Qgs3DTypes::AltClampAbsolute || altClamp == Qgs3DTypes::AltClampRelative )
      geomZ = lineString->zAt( i );

    float z = ( terrainZ + geomZ ) * map.terrainVerticalScale() + height;
    lineString->setZAt( i, z );
  }
}


bool Qgs3DUtils::clampAltitudes( QgsPolygon *polygon, Qgs3DTypes::AltitudeClamping altClamp, Qgs3DTypes::AltitudeBinding altBind, float height, const Qgs3DMapSettings &map )
{
  if ( !polygon->is3D() )
    polygon->addZValue( 0 );

  QgsPoint centroid;
  if ( altBind == Qgs3DTypes::AltBindCentroid )
    centroid = polygon->centroid();

  QgsCurve *curve = const_cast<QgsCurve *>( polygon->exteriorRing() );
  QgsLineString *lineString = qgsgeometry_cast<QgsLineString *>( curve );
  if ( !lineString )
    return false;

  clampAltitudes( lineString, altClamp, altBind, centroid, height, map );

  for ( int i = 0; i < polygon->numInteriorRings(); ++i )
  {
    QgsCurve *curve = const_cast<QgsCurve *>( polygon->interiorRing( i ) );
    QgsLineString *lineString = qgsgeometry_cast<QgsLineString *>( curve );
    if ( !lineString )
      return false;

    clampAltitudes( lineString, altClamp, altBind, centroid, height, map );
  }
  return true;
}


QString Qgs3DUtils::matrix4x4toString( const QMatrix4x4 &m )
{
  const float *d = m.constData();
  QStringList elems;
  elems.reserve( 16 );
  for ( int i = 0; i < 16; ++i )
    elems << QString::number( d[i] );
  return elems.join( ' ' );
}

QMatrix4x4 Qgs3DUtils::stringToMatrix4x4( const QString &str )
{
  QMatrix4x4 m;
  float *d = m.data();
  QStringList elems = str.split( ' ' );
  for ( int i = 0; i < 16; ++i )
    d[i] = elems[i].toFloat();
  return m;
}

void Qgs3DUtils::extractPointPositions( QgsFeature &f, const Qgs3DMapSettings &map, Qgs3DTypes::AltitudeClamping altClamp, QVector<QVector3D> &positions )
{
  const QgsAbstractGeometry *g = f.geometry().constGet();
  for ( auto it = g->vertices_begin(); it != g->vertices_end(); ++it )
  {
    QgsPoint pt = *it;
    float geomZ = 0;
    if ( pt.is3D() )
    {
      geomZ = pt.z();
    }
    float terrainZ = map.terrainGenerator()->heightAt( pt.x(), pt.y(), map ) * map.terrainVerticalScale();
    float h;
    switch ( altClamp )
    {
      case Qgs3DTypes::AltClampAbsolute:
      default:
        h = geomZ;
        break;
      case Qgs3DTypes::AltClampTerrain:
        h = terrainZ;
        break;
      case Qgs3DTypes::AltClampRelative:
        h = terrainZ + geomZ;
        break;
    }
    positions.append( QVector3D( pt.x() - map.origin().x(), h, -( pt.y() - map.origin().y() ) ) );
    //qDebug() << positions.last();
  }
}

/**
 * copied from https://searchcode.com/codesearch/view/35195518/
 * qt3d /src/threed/painting/qglpainter.cpp
 * no changes in the code
 */
static inline uint outcode( QVector4D v )
{
  // For a discussion of outcodes see pg 388 Dunn & Parberry.
  // For why you can't just test if the point is in a bounding box
  // consider the case where a view frustum with view-size 1.5 x 1.5
  // is tested against a 2x2 box which encloses the near-plane, while
  // all the points in the box are outside the frustum.
  // TODO: optimise this with assembler - according to D&P this can
  // be done in one line of assembler on some platforms
  uint code = 0;
  if ( v.x() < -v.w() ) code |= 0x01;
  if ( v.x() > v.w() )  code |= 0x02;
  if ( v.y() < -v.w() ) code |= 0x04;
  if ( v.y() > v.w() )  code |= 0x08;
  if ( v.z() < -v.w() ) code |= 0x10;
  if ( v.z() > v.w() )  code |= 0x20;
  return code;
}


/**
 * coarse box vs frustum test for culling.
 * corners of oriented box are transformed to clip space
 * and there is a test that all points are on the wrong side of the same plane
 * see http://www.lighthouse3d.com/tutorials/view-frustum-culling/geometric-approach-testing-boxes/
 *
 * should be equivalent to https://searchcode.com/codesearch/view/35195518/
 * qt3d /src/threed/painting/qglpainter.cpp
 * bool QGLPainter::isCullable(const QBox3D& box) const
 */
bool Qgs3DUtils::isCullable( const QgsAABB &bbox, const QMatrix4x4 &viewProjectionMatrix )
{
  uint out = 0xff;

  for ( int i = 0; i < 8; ++i )
  {
    QVector4D p( ( ( i >> 0 ) & 1 ) ? bbox.xMin : bbox.xMax,
                 ( ( i >> 1 ) & 1 ) ? bbox.yMin : bbox.yMax,
                 ( ( i >> 2 ) & 1 ) ? bbox.zMin : bbox.zMax, 1 );
    QVector4D pc = viewProjectionMatrix * p;

    // if the logical AND of all the outcodes is non-zero then the BB is
    // definitely outside the view frustum.
    out = out & outcode( pc );
  }
  return out;
}

QgsVector3D Qgs3DUtils::mapToWorldCoordinates( const QgsVector3D &mapCoords, const QgsVector3D &origin )
{
  return QgsVector3D( mapCoords.x() - origin.x(),
                      mapCoords.z() - origin.z(),
                      -( mapCoords.y() - origin.y() ) );

}

QgsVector3D Qgs3DUtils::worldToMapCoordinates( const QgsVector3D &worldCoords, const QgsVector3D &origin )
{
  return QgsVector3D( worldCoords.x() + origin.x(),
                      -worldCoords.z() + origin.y(),
                      worldCoords.y() + origin.z() );
}

static QgsRectangle _tryReprojectExtent2D( const QgsRectangle &extent, const QgsCoordinateReferenceSystem &crs1, const QgsCoordinateReferenceSystem &crs2, const QgsCoordinateTransformContext &context )
{
  QgsRectangle extentMapCrs( extent );
  if ( crs1 != crs2 )
  {
    // reproject if necessary
    QgsCoordinateTransform ct( crs1, crs2, context );
    try
    {
      extentMapCrs = ct.transformBoundingBox( extentMapCrs );
    }
    catch ( const QgsCsException & )
    {
      // bad luck, can't reproject for some reason
      QgsDebugMsg( QStringLiteral( "3D utils: transformation of extent failed: " ) + extentMapCrs.toString( -1 ) );
    }
  }
  return extentMapCrs;
}

QgsAABB Qgs3DUtils::layerToWorldExtent( const QgsRectangle &extent, double zMin, double zMax, const QgsCoordinateReferenceSystem &layerCrs, const QgsVector3D &mapOrigin, const QgsCoordinateReferenceSystem &mapCrs, const QgsCoordinateTransformContext &context )
{
  QgsRectangle extentMapCrs( _tryReprojectExtent2D( extent, layerCrs, mapCrs, context ) );
  return mapToWorldExtent( extentMapCrs, zMin, zMax, mapOrigin );
}

QgsRectangle Qgs3DUtils::worldToLayerExtent( const QgsAABB &bbox, const QgsCoordinateReferenceSystem &layerCrs, const QgsVector3D &mapOrigin, const QgsCoordinateReferenceSystem &mapCrs, const QgsCoordinateTransformContext &context )
{
  QgsRectangle extentMap = worldToMapExtent( bbox, mapOrigin );
  return _tryReprojectExtent2D( extentMap, mapCrs, layerCrs, context );
}

QgsAABB Qgs3DUtils::mapToWorldExtent( const QgsRectangle &extent, double zMin, double zMax, const QgsVector3D &mapOrigin )
{
  QgsVector3D extentMin3D( extent.xMinimum(), extent.yMinimum(), zMin );
  QgsVector3D extentMax3D( extent.xMaximum(), extent.yMaximum(), zMax );
  QgsVector3D worldExtentMin3D = mapToWorldCoordinates( extentMin3D, mapOrigin );
  QgsVector3D worldExtentMax3D = mapToWorldCoordinates( extentMax3D, mapOrigin );
  QgsAABB rootBbox( worldExtentMin3D.x(), worldExtentMin3D.y(), worldExtentMin3D.z(),
                    worldExtentMax3D.x(), worldExtentMax3D.y(), worldExtentMax3D.z() );
  return rootBbox;
}

QgsRectangle Qgs3DUtils::worldToMapExtent( const QgsAABB &bbox, const QgsVector3D &mapOrigin )
{
  QgsVector3D worldExtentMin3D = Qgs3DUtils::worldToMapCoordinates( QgsVector3D( bbox.xMin, bbox.yMin, bbox.zMin ), mapOrigin );
  QgsVector3D worldExtentMax3D = Qgs3DUtils::worldToMapCoordinates( QgsVector3D( bbox.xMax, bbox.yMax, bbox.zMax ), mapOrigin );
  QgsRectangle extentMap( worldExtentMin3D.x(), worldExtentMin3D.y(), worldExtentMax3D.x(), worldExtentMax3D.y() );
  // we discard zMin/zMax here because we don't need it
  return extentMap;
}


QgsVector3D Qgs3DUtils::transformWorldCoordinates( const QgsVector3D &worldPoint1, const QgsVector3D &origin1, const QgsCoordinateReferenceSystem &crs1, const QgsVector3D &origin2, const QgsCoordinateReferenceSystem &crs2, const QgsCoordinateTransformContext &context )
{
  QgsVector3D mapPoint1 = worldToMapCoordinates( worldPoint1, origin1 );
  QgsVector3D mapPoint2 = mapPoint1;
  if ( crs1 != crs2 )
  {
    // reproject if necessary
    QgsCoordinateTransform ct( crs1, crs2, context );
    try
    {
      QgsPointXY pt = ct.transform( QgsPointXY( mapPoint1.x(), mapPoint1.y() ) );
      mapPoint2.set( pt.x(), pt.y(), mapPoint1.z() );
    }
    catch ( const QgsCsException & )
    {
      // bad luck, can't reproject for some reason
    }
  }
  return mapToWorldCoordinates( mapPoint2, origin2 );
}

void Qgs3DUtils::estimateVectorLayerZRange( QgsVectorLayer *layer, double &zMin, double &zMax )
{
  if ( !QgsWkbTypes::hasZ( layer->wkbType() ) )
  {
    zMin = 0;
    zMax = 0;
    return;
  }

  zMin = std::numeric_limits<double>::max();
  zMax = std::numeric_limits<double>::min();

  QgsFeature f;
  QgsFeatureIterator it = layer->getFeatures( QgsFeatureRequest().setNoAttributes().setLimit( 100 ) );
  while ( it.nextFeature( f ) )
  {
    QgsGeometry g = f.geometry();
    for ( auto vit = g.vertices_begin(); vit != g.vertices_end(); ++vit )
    {
      double z = ( *vit ).z();
      if ( z < zMin ) zMin = z;
      if ( z > zMax ) zMax = z;
    }
  }

  if ( zMin == std::numeric_limits<double>::max() && zMax == std::numeric_limits<double>::min() )
  {
    zMin = 0;
    zMax = 0;
  }
}

std::unique_ptr<QgsAbstract3DSymbol> Qgs3DUtils::symbolForGeometryType( QgsWkbTypes::GeometryType geomType )
{
  switch ( geomType )
  {
    case QgsWkbTypes::PointGeometry:
      return std::unique_ptr<QgsAbstract3DSymbol>( new QgsPoint3DSymbol );
    case QgsWkbTypes::LineGeometry:
      return std::unique_ptr<QgsAbstract3DSymbol>( new QgsLine3DSymbol );
    case QgsWkbTypes::PolygonGeometry:
      return std::unique_ptr<QgsAbstract3DSymbol>( new QgsPolygon3DSymbol );
    default:
      return nullptr;
  }
}

QgsExpressionContext Qgs3DUtils::globalProjectLayerExpressionContext( QgsVectorLayer *layer )
{
  QgsExpressionContext exprContext;
  exprContext << QgsExpressionContextUtils::globalScope()
              << QgsExpressionContextUtils::projectScope( QgsProject::instance() )
              << QgsExpressionContextUtils::layerScope( layer );
  return exprContext;
}

Qt3DExtras::QPhongMaterial *Qgs3DUtils::phongMaterial( const QgsPhongMaterialSettings &settings )
{
  Qt3DExtras::QPhongMaterial *phong = new Qt3DExtras::QPhongMaterial;
  phong->setAmbient( settings.ambient() );
  phong->setDiffuse( settings.diffuse() );
  phong->setSpecular( settings.specular() );
  phong->setShininess( settings.shininess() );
  return phong;
}
