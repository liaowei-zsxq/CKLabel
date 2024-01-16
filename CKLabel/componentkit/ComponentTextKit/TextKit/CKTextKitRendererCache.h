/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#import <Foundation/Foundation.h>

#import "CKCacheImpl.h"
#import "CKTextKitAttributes.h"

@class CKTextKitRenderer;

namespace CK {
  namespace TextKit {
    /*
     These are wrapper functions for CFNotificationCenter to call into the cache with low memory and backgrounding
     notifications.
     */
    void lowMemoryNotificationHandler(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef userInfo);
    void enteredBackgroundNotificationHandler(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef userInfo);

    struct ApplicationObserver {
      std::function<void (void)>onLowMemory;
      std::function<void (void)>onEnterBackground;
      ApplicationObserver(std::function<void (void)> lowMem, std::function<void (void)>bg) :
      onLowMemory(lowMem), onEnterBackground(bg) {
        // We use CFNotificationCenter here so that we can avoid creating an NSObject and registering it *just* to
        // receive low memory and backgrounding notifications.
        CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(),
                                        this,
                                        &lowMemoryNotificationHandler,
                                        (__bridge CFStringRef)UIApplicationDidReceiveMemoryWarningNotification,
                                        NULL,
                                        CFNotificationSuspensionBehaviorDeliverImmediately);
        CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(),
                                        this,
                                        &enteredBackgroundNotificationHandler,
                                        (__bridge CFStringRef)UIApplicationDidEnterBackgroundNotification,
                                        NULL,
                                        CFNotificationSuspensionBehaviorDeliverImmediately);
      };
      ApplicationObserver() {
        CFNotificationCenterRemoveObserver(CFNotificationCenterGetLocalCenter(),
                                           this,
                                           (__bridge CFStringRef)UIApplicationDidReceiveMemoryWarningNotification,
                                           NULL);
        CFNotificationCenterRemoveObserver(CFNotificationCenterGetLocalCenter(),
                                           this,
                                           (__bridge CFStringRef)UIApplicationDidEnterBackgroundNotification,
                                           NULL);
      };
    };

    namespace Renderer {
      /**
       This cache key is conceptually different from the TextComponent Attributes.  It must contain everything
       related to the actual drawing of the text, which may include additional parameters.
       */
      struct Key {
        UIUserInterfaceStyle userInterfaceStyle;
        CKTextKitAttributes attributes;
        CGSize constrainedSize;

        Key(UIUserInterfaceStyle userInterfaceStyle, CKTextKitAttributes a, CGSize cs);

        size_t hash;

        bool operator==(const Key &other) const
        {
          // These comparisons are in a specific order to reduce the overall cost of this function.
          return hash == other.hash
          && CGSizeEqualToSize(constrainedSize, other.constrainedSize)
          && attributes == other.attributes
          && userInterfaceStyle == other.userInterfaceStyle;
        }
      };

      struct KeyHasher {
        size_t operator()(const Key &k) const
        {
          return k.hash;
        }
      };

      /*
       This is a thin wrapper around a c++ cache and a mutex.  It wraps the bare minimum of calls we need for this case
       with a simple mutex, and observes for memory warnings and backgrounding notifications so that we compact or evict
       the cache.

       These caches are very useful for:

       1. Performance of text layout.  You can cache CKTextKitRenderer objects which store very expensive to compute TextKit layout
       artifacts.  The cache is in C++ so we can use stack-allocated keys to query the cache and avoid additional
       mallocs just to fetch a pre-built renderer or raster buffers.

       2. Performance of text rendering.  You can cache the raster CGImageRefs or UIImages generated by a renderer
       object using a cache and not redraw commonly used text (think "Like" button text...).

       3. Memory profile.  Text artifacts and raster buffers are LARGE.  The best practice to reduce overall memory load
       is actually not to hold onto your renderer objects directly, but instead just hold onto CKTextKitAttributes
       struct and query a renderer or raster cache when the results are needed.  Since these caches can maintain a
       central, threadsafe data structure of all artifacts it can use a LRU policy to evict less active artifacts over
       the lifetime of the application.  What this means is that you get a small, stable memory footprint of your text
       in your application, no matter how many different text elements you may be drawing.  The maximum cost factor
       should be tuned based on which artifacts you're storing in this cache.  If you are storing raster buffers then it
       should likely be a couple MB.  If you are storing renderers it's a good idea to have it related to the visible
       length of the string (as a proxy for number of glyph artifacts).  For an example of usage please see ASTextNode
       or CKTextComponent.
       */
      struct Cache {
      private:
        CK::ConcurrentCacheImpl<const Key, id, KeyHasher> cache;
        ApplicationObserver *applicationObserver;

      public:
        Cache(const std::string cacheName, const NSUInteger maxCost, const CGFloat compactionFactor) : cache(cacheName, maxCost, compactionFactor) {
          applicationObserver = new ApplicationObserver([this] {
            compact(0.95);
          }, [this] {
            removeAllObjects();
          });
        };

        ~Cache() {
          delete applicationObserver;
        }

        void cacheObject(const Key &key, id object, size_t cost) {
          cache.insert(key, object, cost);
        }

        id objectForKey(const Key &key) {
          return cache.find(key);
        }

        void compact(double compactionFactor) {
          cache.compact(compactionFactor);
        }

        void removeAllObjects() {
          cache.removeAllObjects();
        }
      };
    };
  };
};
