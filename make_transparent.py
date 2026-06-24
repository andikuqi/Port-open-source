import sys
import math
from PIL import Image

def process_image(path):
    print(f"Processing {path}...")
    img = Image.open(path).convert("RGBA")
    pixels = img.load()
    width, height = img.size
    
    # Target background color to remove: hex 0d1220 (13, 18, 32)
    bg_r, bg_g, bg_b = 13, 18, 32
    
    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            
            # Distance from background color
            dist = math.sqrt((r - bg_r)**2 + (g - bg_g)**2 + (b - bg_b)**2)
            
            # Distance from pure black
            dist_black = math.sqrt(r**2 + g**2 + b**2)
            
            min_dist = min(dist, dist_black)
            
            if min_dist < 25:
                pixels[x, y] = (0, 0, 0, 0)
            elif min_dist < 65:
                # Smooth alpha transition to prevent jagged edges
                alpha = int((min_dist - 25) / 40.0 * 255)
                pixels[x, y] = (r, g, b, min(a, alpha))
                
    img.save(path)

if __name__ == "__main__":
    process_image("myport-icon.png")
    process_image("bin-empty.png")
    process_image("bin-full.png")
    print("Transparency applied successfully!")
