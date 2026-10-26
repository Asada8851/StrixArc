#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
// CoreImage blur and convolution filters on the default (Metal) context, read along the centre row of a blue/white
// picture: a zero row means the filter produced nothing. Usage: ciprobe [filter name ...]
static void show(const char *what, CIContext *ctx, CIImage *img) {
    uint8_t *px = calloc(256 * 4, 1);
    [ctx render:img toBitmap:px rowBytes:256 * 4 bounds:CGRectMake(0, 128, 256, 1) format:kCIFormatRGBA8 colorSpace:CGColorSpaceCreateWithName(kCGColorSpaceSRGB)];
    printf("%-30s x=32 %3d %3d %3d %3d | x=126 %3d %3d %3d | x=130 %3d %3d %3d\n", what,
           px[32*4], px[32*4+1], px[32*4+2], px[32*4+3], px[126*4], px[126*4+1], px[126*4+2], px[130*4], px[130*4+1], px[130*4+2]);
    free(px);
}
int main(int argc, char **argv) { @autoreleasepool {
    CIContext *ctx = [CIContext contextWithOptions:@{ kCIContextCacheIntermediates: @NO }];
    CIImage *blue = [[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:1]] imageByCroppingToRect:CGRectMake(0, 0, 128, 256)];
    CIImage *white = [[CIImage imageWithColor:[CIColor colorWithRed:1 green:1 blue:1]] imageByCroppingToRect:CGRectMake(128, 0, 128, 256)];
    CIImage *pic = [blue imageByCompositingOverImage:white], *cl = pic.imageByClampingToExtent;
    for (NSNumber *r in argc > 1 ? @[] : @[@0.3, @0.6, @1, @2, @3, @5, @16]) {
        CIFilter *b = [CIFilter filterWithName:@"CIGaussianBlur"]; [b setValue:cl forKey:kCIInputImageKey]; [b setValue:r forKey:@"inputRadius"];
        show([NSString stringWithFormat:@"gaussian r=%@", r].UTF8String, ctx, [b.outputImage imageByCroppingToRect:pic.extent]);
    }
    const CGFloat w3[9] = {1,2,1,2,4,2,1,2,1}, w9[9] = {1,1,1,1,1,1,1,1,1};
    NSMutableArray *names = [NSMutableArray array]; for (int i = 1; i < argc; i++) [names addObject:@(argv[i])];
    for (NSString *f in argc > 1 ? names : @[@"CIConvolution3X3", @"CIConvolution5X5", @"CIConvolution7X7", @"CIConvolution9Horizontal", @"CIConvolution9Vertical"]) {
        CIFilter *c = [CIFilter filterWithName:f]; [c setValue:cl forKey:kCIInputImageKey];
        int n = [f hasSuffix:@"3X3"] ? 9 : [f hasSuffix:@"5X5"] ? 25 : [f hasSuffix:@"7X7"] ? 49 : 9;
        CGFloat *w = calloc(n, sizeof(CGFloat)); for (int i = 0; i < n; i++) w[i] = (n == 9 && [f hasSuffix:@"3X3"] ? w3[i] / 16.0 : 1.0 / n);
        (void)w9;
        [c setValue:[CIVector vectorWithValues:w count:n] forKey:@"inputWeights"]; free(w);
        show(f.UTF8String, ctx, [c.outputImage imageByCroppingToRect:pic.extent]);
    }
} return 0; }
